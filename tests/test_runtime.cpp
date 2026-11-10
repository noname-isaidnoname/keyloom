#include "harness.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <linux/input.h>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "backends.hpp"
#include "keys.hpp"
#include "parser.hpp"
#include "runtime.hpp"
#include "scheduler.hpp"
#include "subprocess.hpp"
#include "unique_fd.hpp"

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

struct FailFirstReleaseBackend : RecordingBackend {
    bool fail_key = true;
    bool fail_button = true;
    void key(const std::string& name, bool down) override {
        if (!down && fail_key) {
            fail_key = false;
            throw BackendError("key release failed");
        }
        RecordingBackend::key(name, down);
    }
    void button(const std::string& name, bool down) override {
        if (!down && fail_button) {
            fail_button = false;
            throw BackendError("button release failed");
        }
        RecordingBackend::button(name, down);
    }
};

struct CaptureOnDestroyBackend : RecordingBackend {
    explicit CaptureOnDestroyBackend(std::vector<std::string>& output) : output_(output) {}
    ~CaptureOnDestroyBackend() override { output_ = log; }
    std::vector<std::string>& output_;
};

struct ThrowCloseBackend : RecordingBackend {
    int closes = 0;
    void close() override {
        ++closes;
        throw BackendError("injected close failure");
    }
};

struct ObserveCloseBackend : RecordingBackend {
    explicit ObserveCloseBackend(bool& dependency_done) : dependency_done_(dependency_done) {}
    bool closed_after_dependency = false;
    void close() override {
        closed_after_dependency = dependency_done_;
        RecordingBackend::close();
    }
    bool& dependency_done_;
};

Task slow_cleanup_dependency(bool& finished) {
    co_await sleep_for(0.08);
    finished = true;
}

Task stop_then_cancel_runtime(Runtime& runtime, Task::handle_type root) {
    co_await sleep_for(0.01);
    runtime.scheduler().request_stop();
    co_await sleep_for(0.01);
    runtime.scheduler().cancel(root);
}

Task stop_runtime_soon(Runtime& runtime) {
    co_await sleep_for(0.01);
    runtime.scheduler().request_stop();
}

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

struct FailingInput : VirtualInput {
    RecordedInput& recorder;
    bool fail_release = false;
    bool fail_flush = false;
    explicit FailingInput(RecordedInput& target) : recorder(target) {}
    void write(int type, int code, int value) override {
        if (fail_release && type == EV_KEY && value == 0) {
            fail_release = false;
            throw BackendError("injected release failure");
        }
        recorder.write(type, code, value);
    }
    void flush() override {
        if (fail_flush) {
            fail_flush = false;
            throw BackendError("injected flush failure");
        }
        recorder.flush();
    }
    void close() override { recorder.close(); }
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

Task release_test_key(Runtime& runtime) {
    co_await sleep_for(0.04);
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

TEST_CASE(suspend_hotkeys_keeps_the_calling_hotkey_enabled) {
    const Script script = parse(R"LOOM(
loom "suspend" {
    when key F9 goes down {
        if hotkeys_suspended() {
            call suspend_hotkeys(false)
        } else {
            call suspend_hotkeys(true)
        }
    }
    when key F8 goes down {
        send "F8"
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.physical_key("fake-keyboard", "F8", true);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "F8", false);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "F9", true);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "F9", false);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "F8", true);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "F8", false);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "F9", true);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "F9", false);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "F8", true);
    runtime.scheduler().run();

    CHECK_EQ(raw->log, std::vector<std::string>({"type F8", "type F8"}));
}

TEST_CASE(set_hotkey_enabled_affects_only_the_selected_binding) {
    const Script script = parse(R"LOOM(
loom "toggle-binding" {
    when key F7 goes down { call set_hotkey_enabled("F8", false) }
    when key F6 goes down { call set_hotkey_enabled("F8", true) }
    when key F8 goes down { send "F8" }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    const auto press = [&runtime](const std::string& key) {
        runtime.physical_key("fake-keyboard", key, true);
        runtime.scheduler().run();
        runtime.physical_key("fake-keyboard", key, false);
        runtime.scheduler().run();
    };
    press("F8");
    press("F7");
    press("F8");
    press("F6");
    press("F8");

    CHECK_EQ(raw->log, std::vector<std::string>({"type F8", "type F8"}));
}

TEST_CASE(disabled_hotkeys_do_not_arm_up_bindings) {
    const Script script = parse(R"LOOM(
loom "disabled-up" {
    when key F7 goes down { call set_hotkey_enabled("F8", false) }
    when key F6 goes down { call set_hotkey_enabled("F8", true) }
    when key F8 goes up { send "F8UP" }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    const auto press = [&runtime](const std::string& key) {
        runtime.physical_key("fake-keyboard", key, true);
        runtime.scheduler().run();
        runtime.physical_key("fake-keyboard", key, false);
        runtime.scheduler().run();
    };
    press("F8");   // enabled: arms and fires up
    press("F7");   // disables F8
    press("F8");   // disabled: must not arm or fire
    press("F6");   // re-enables F8
    press("F8");   // enabled again

    CHECK_EQ(raw->log, std::vector<std::string>({"type F8UP", "type F8UP"}));
}

TEST_CASE(suspended_hotkeys_do_not_arm_up_bindings) {
    const Script script = parse(R"LOOM(
loom "suspended-up" {
    when key F9 goes down { call suspend_hotkeys(not hotkeys_suspended()) }
    when key F8 goes up { send "F8UP" }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    const auto press = [&runtime](const std::string& key) {
        runtime.physical_key("fake-keyboard", key, true);
        runtime.scheduler().run();
        runtime.physical_key("fake-keyboard", key, false);
        runtime.scheduler().run();
    };
    press("F8");   // not suspended
    press("F9");   // suspend (F9 exempt)
    press("F8");   // suspended: must not arm or fire
    press("F9");   // resume
    press("F8");   // resumed

    CHECK_EQ(raw->log, std::vector<std::string>({"type F8UP", "type F8UP"}));
}

TEST_CASE(gui_initialization_failure_cleans_backend_and_preserves_primary_error) {
    const char* display = std::getenv("DISPLAY");
    const bool had_display = display != nullptr;
    const std::string original = had_display ? display : "";
    ::setenv("DISPLAY", "invalid-keyloom-display", 1);
    Runtime runtime(parse(R"LOOM(
loom "startup-failure" {
    gui "panel" { label "hello" }
    when app starts { log "ready" }
}
)LOOM"));
    auto backend = std::make_unique<ThrowCloseBackend>();
    auto* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.scheduler().spawn(runtime.run());
    runtime.scheduler().run();
    if (had_display) ::setenv("DISPLAY", original.c_str(), 1);
    else ::unsetenv("DISPLAY");

    CHECK_EQ(raw->closes, 1);
    CHECK_TRUE(runtime.failure() != nullptr);
    std::string message;
    try {
        std::rethrow_exception(runtime.failure());
    } catch (const std::exception& error) {
        message = error.what();
    }
    CHECK_TRUE(message.find("needs an X11 display") != std::string::npos);
}

TEST_CASE(cancellation_during_cleanup_still_drains_before_closing_backend) {
    Runtime runtime(parse("loom \"cleanup-cancel\" { when app starts { log \"ready\" } }"));
    bool dependency_done = false;
    auto backend = std::make_unique<ObserveCloseBackend>(dependency_done);
    auto* raw = backend.get();
    runtime.set_backend(std::move(backend));
    Task root = runtime.run();
    runtime.scheduler().spawn(root);
    runtime.scheduler().spawn(slow_cleanup_dependency(dependency_done));
    runtime.scheduler().spawn(stop_then_cancel_runtime(runtime, root.handle));
    runtime.scheduler().run();
    CHECK_TRUE(dependency_done);
    CHECK_TRUE(raw->closed_after_dependency);
    CHECK_TRUE(runtime.failure() == nullptr);
    CHECK_EQ(runtime.scheduler().live_count(), static_cast<size_t>(0));
}

TEST_CASE(backend_close_failure_is_reported_after_shutdown) {
    Runtime runtime(parse("loom \"close-failure\" { when app starts { log \"ready\" } }"));
    auto backend = std::make_unique<ThrowCloseBackend>();
    auto* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.scheduler().spawn(runtime.run());
    runtime.scheduler().spawn(stop_runtime_soon(runtime));
    runtime.scheduler().run();
    CHECK_EQ(raw->closes, 1);
    CHECK_TRUE(runtime.failure() != nullptr);
    CHECK_THROWS(std::rethrow_exception(runtime.failure()), BackendError);
    CHECK_EQ(runtime.scheduler().live_count(), static_cast<size_t>(0));
}

TEST_CASE(runtime_destruction_during_startup_restores_signal_mask) {
    sigset_t before{};
    CHECK_EQ(::sigprocmask(SIG_SETMASK, nullptr, &before), 0);
    {
        Runtime::Options options;
        options.dry_run = true;
        Runtime runtime(parse("loom \"startup\" { when app starts { wait 60s } }"), options);
        runtime.scheduler().spawn(runtime.run());
        runtime.scheduler().run_ready();
        sigset_t during{};
        CHECK_EQ(::sigprocmask(SIG_SETMASK, nullptr, &during), 0);
        CHECK_EQ(::sigismember(&during, SIGINT), 1);
        CHECK_EQ(::sigismember(&during, SIGTERM), 1);
        CHECK_TRUE(runtime.scheduler().live_count() > 0);
    }
    sigset_t after{};
    CHECK_EQ(::sigprocmask(SIG_SETMASK, nullptr, &after), 0);
    CHECK_EQ(::sigismember(&after, SIGINT), ::sigismember(&before, SIGINT));
    CHECK_EQ(::sigismember(&after, SIGTERM), ::sigismember(&before, SIGTERM));
}

TEST_CASE(deferred_event_owns_its_event_snapshot) {
    Script script = parse("loom \"event-owner\" { when app starts { key A press } }");
    Runtime runtime(script);
    auto backend = std::make_unique<RecordingBackend>();
    auto* raw = backend.get();
    runtime.set_backend(std::move(backend));
    Task event = runtime.run_event(script.events.at(0));
    script.events.at(0).body.clear();
    runtime.scheduler().spawn(event);
    runtime.scheduler().run();
    CHECK_EQ(raw->log, std::vector<std::string>({"key A down"}));
}

TEST_CASE(deferred_held_loop_owns_its_event_snapshot) {
    Script script = parse("loom \"held-owner\" { when key X stays held every 10ms { key A tap for 1ms } }");
    Runtime runtime(script);
    auto backend = std::make_unique<RecordingBackend>();
    auto* raw = backend.get();
    runtime.set_backend(std::move(backend));
    Task held = runtime.held_loop(script.events.at(0), "X", 0.01);
    script.events.at(0).body.clear();
    runtime.pressed_keys().insert("X");
    runtime.scheduler().spawn(held);
    runtime.scheduler().spawn(release_test_key(runtime));
    runtime.scheduler().run();
    CHECK_TRUE(std::count(raw->log.begin(), raw->log.end(), "key A down") > 0);
}

TEST_CASE(runtime_destruction_releases_remaining_output_holds) {
    std::vector<std::string> log;
    {
        Runtime runtime(parse("loom \"cleanup\" { when app starts { log \"ready\" } }"));
        runtime.set_backend(std::make_unique<CaptureOnDestroyBackend>(log));
        runtime.hold_key("A");
        runtime.hold_button("owner", "left");
    }
    CHECK_EQ(log, std::vector<std::string>({"key A down", "button left down", "button left up", "key A up", "close"}));
}

TEST_CASE(failed_output_releases_remain_tracked_for_retry) {
    Script script = parse("loom \"retry\" { when app starts { log \"ready\" } }");
    Runtime runtime(script);
    auto backend = std::make_unique<FailFirstReleaseBackend>();
    auto* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.hold_key("A");
    runtime.hold_button("owner", "left");
    CHECK_THROWS(runtime.free_key("A"), BackendError);
    CHECK_THROWS(runtime.free_button("owner"), BackendError);
    runtime.free_key("A");
    runtime.free_button("owner");
    CHECK_EQ(raw->log, std::vector<std::string>({"key A down", "button left down", "key A up", "button left up"}));
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

TEST_CASE(disconnect_releases_remap_after_last_device) {
    const Script script = parse(R"LOOM(
loom "disconnect" {
    remap key Q to CTRL+SPACE
    when app starts { log "ready" }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");
    runtime.physical_key("one", "Q", true);
    runtime.physical_key("two", "Q", true);
    runtime.disconnect_device("one");
    CHECK_EQ(raw->log, std::vector<std::string>({"key LEFTCTRL down", "key SPACE down"}));
    runtime.disconnect_device("two");
    runtime.disconnect_device("two");
    CHECK_EQ(raw->log, std::vector<std::string>({"key LEFTCTRL down", "key SPACE down", "key SPACE up", "key LEFTCTRL up"}));
    CHECK_TRUE(runtime.pressed_keys().empty());
}

TEST_CASE(evdev_overflow_resynchronizes_remaps_and_passthrough) {
    const Script script = parse(R"LOOM(
loom "overflow" {
    remap key Q to SPACE
    when key B goes down { key C press }
}
)LOOM");
    Runtime runtime(script);
    auto backend = std::make_unique<RecordingBackend>();
    auto* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");
    runtime.set_consumed_keys(runtime.collect_consumed_keys());
    FakeSink sink;
    const std::string path = "keyboard";
    runtime.process_events({{EV_KEY, KEY_Q, 1}, {EV_KEY, KEY_A, 1}, {EV_SYN, SYN_REPORT, 0}}, &sink, path);
    runtime.process_events({{EV_SYN, SYN_DROPPED, 0}, {EV_KEY, KEY_B, 0}}, &sink, path);
    CHECK_EQ(raw->log, std::vector<std::string>({"key SPACE down"}));
    int snapshots = 0;
    runtime.process_events({{EV_KEY, KEY_A, 1}, {EV_SYN, SYN_REPORT, 0}}, &sink, path, [&] {
        ++snapshots;
        return std::vector<int>{KEY_B};
    });
    runtime.scheduler().run();
    CHECK_EQ(snapshots, 1);
    CHECK_EQ(raw->log, std::vector<std::string>({"key SPACE down", "key SPACE up", "key C down"}));
    CHECK_TRUE(runtime.pressed_keys().count("Q") == 0);
    CHECK_TRUE(runtime.pressed_keys().count("A") == 0);
    CHECK_TRUE(runtime.pressed_keys().count("B") == 1);
    CHECK_EQ(sink.writes, std::vector<std::string>({"1:30:1", "0:0:0", "1:30:0", "1:48:1", "0:0:0"}));
}

TEST_CASE(evdev_overflow_snapshot_failure_stops_runtime) {
    Runtime runtime(parse("loom \"overflow\" { when app starts { log \"ready\" } }"));
    runtime.process_events({{EV_SYN, SYN_DROPPED, 0}, {EV_SYN, SYN_REPORT, 0}}, nullptr, "keyboard");
    CHECK_TRUE(runtime.scheduler().stop_requested());
    CHECK_TRUE(runtime.runtime_error().find("resynchronization failed") != std::string::npos);
}

TEST_CASE(evdev_overflow_ignores_stale_events_already_in_read_batch) {
    const Script script = parse(R"LOOM(
loom "overflow-tail" {
    remap key Q to SPACE
    when app starts { log "ready" }
}
)LOOM");
    Runtime runtime(script);
    auto backend = std::make_unique<RecordingBackend>();
    auto* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");
    runtime.set_consumed_keys(runtime.collect_consumed_keys());
    FakeSink sink;
    // The stale tail predates the EVIOCGKEY snapshot, so its keys and its
    // non-key events (including the trailing SYN_REPORT) must not be replayed.
    runtime.process_events({{EV_SYN, SYN_DROPPED, 0}, {EV_SYN, SYN_REPORT, 0},
                            {EV_KEY, KEY_Q, 0}, {EV_KEY, KEY_A, 1}, {EV_REL, REL_X, 7},
                            {EV_SYN, SYN_REPORT, 0}},
                           &sink, "keyboard", [] { return std::vector<int>{KEY_Q}; });
    CHECK_EQ(raw->log, std::vector<std::string>({"key SPACE down"}));
    CHECK_TRUE(runtime.pressed_keys().count("Q") == 1);
    CHECK_TRUE(runtime.pressed_keys().count("A") == 0);
    CHECK_EQ(sink.writes, std::vector<std::string>({"0:0:0"}));

    // A subsequent read contains events queued after the snapshot, not its stale tail.
    runtime.process_events({{EV_KEY, KEY_Q, 0}, {EV_KEY, KEY_A, 1}, {EV_SYN, SYN_REPORT, 0}},
                           &sink, "keyboard");
    CHECK_EQ(raw->log, std::vector<std::string>({"key SPACE down", "key SPACE up"}));
    CHECK_TRUE(runtime.pressed_keys().count("Q") == 0);
    CHECK_TRUE(runtime.pressed_keys().count("A") == 1);
}

TEST_CASE(remap_only_script_requires_an_accessible_keyboard) {
    Script script = parse(R"LOOM(
loom "remap-only" {
    remap key Q to SPACE
    when timer every 1s { log "tick" }
}
)LOOM");
    Runtime::Options options;
    options.dry_run = true;
    options.input_device = "/nonexistent-keyloom-input-device";
    Runtime runtime(script, options);
    runtime.scheduler().spawn(runtime.run());
    runtime.scheduler().run();
    CHECK_TRUE(runtime.failure() != nullptr);
    std::string message;
    try { std::rethrow_exception(runtime.failure()); }
    catch (const std::exception& error) { message = error.what(); }
    CHECK_TRUE(message.find("No accessible keyboard devices") != std::string::npos);
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

TEST_CASE(passthrough_factory_must_return_a_device) {
    CHECK_THROWS(PassthroughDevice({}, [](const std::string&, const std::string&, const DeviceCapabilities&) {
        return std::unique_ptr<VirtualInput>();
    }), BackendError);
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

TEST_CASE(passthrough_retries_unconfirmed_release_on_disconnect) {
    RecordedInput recorder;
    FailingInput* input = nullptr;
    PassthroughDevice device({}, [&](const std::string&, const std::string&, const DeviceCapabilities&) {
        auto result = std::make_unique<FailingInput>(recorder);
        input = result.get();
        return result;
    });
    device.write_event(EV_KEY, KEY_A, 1);
    device.write_event(EV_SYN, SYN_REPORT, 0);
    input->fail_release = true;
    CHECK_THROWS(device.write_event(EV_KEY, KEY_A, 0), BackendError);
    device.close();
    CHECK_EQ(recorder.writes, std::vector<std::string>({"1:30:1", "0:0:0", "flush", "1:30:0", "0:0:0", "flush"}));
    CHECK_EQ(recorder.closes, 1);
}

TEST_CASE(passthrough_retries_release_after_failed_report_flush) {
    RecordedInput recorder;
    FailingInput* input = nullptr;
    PassthroughDevice device({}, [&](const std::string&, const std::string&, const DeviceCapabilities&) {
        auto result = std::make_unique<FailingInput>(recorder);
        input = result.get();
        return result;
    });
    device.write_event(EV_KEY, KEY_A, 1);
    device.write_event(EV_SYN, SYN_REPORT, 0);
    device.write_event(EV_KEY, KEY_A, 0);
    input->fail_flush = true;
    CHECK_THROWS(device.write_event(EV_SYN, SYN_REPORT, 0), BackendError);
    device.close();
    CHECK_EQ(std::count(recorder.writes.begin(), recorder.writes.end(), "1:30:0"), 2);
    CHECK_EQ(recorder.closes, 1);
}

TEST_CASE(passthrough_forgets_only_successfully_flushed_releases) {
    RecordedInput recorder;
    PassthroughDevice device({}, [&recorder](const std::string&, const std::string&, const DeviceCapabilities&) {
        return std::make_unique<ForwardingInput>(&recorder);
    });
    device.write_event(EV_KEY, KEY_A, 1);
    device.write_event(EV_KEY, KEY_A, 1);
    device.write_event(EV_SYN, SYN_REPORT, 0);
    device.write_event(EV_KEY, KEY_A, 0);
    device.write_event(EV_SYN, SYN_REPORT, 0);
    device.close();
    CHECK_EQ(std::count(recorder.writes.begin(), recorder.writes.end(), "1:30:0"), 1);
}

TEST_CASE(observed_device_startup_seeds_key_state_without_firing_bindings) {
    Script script = parse(R"LOOM(
loom "already-held" {
    when key A goes down { key B press }
    when key A goes up { key C press }
}
)LOOM");
    Runtime runtime(script);
    auto backend = std::make_unique<RecordingBackend>();
    auto* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");
    runtime.seed_device_keys("one", {KEY_A});
    runtime.seed_device_keys("two", {KEY_A});
    CHECK_TRUE(runtime.pressed_keys().count("A") == 1);
    runtime.process_events({{EV_KEY, KEY_A, 0}}, nullptr, "one");
    CHECK_TRUE(runtime.pressed_keys().count("A") == 1);
    runtime.process_events({{EV_KEY, KEY_A, 0}}, nullptr, "two");
    runtime.scheduler().run();
    CHECK_TRUE(runtime.pressed_keys().empty());
    CHECK_TRUE(raw->log.empty());
    runtime.process_events({{EV_KEY, KEY_A, 1}, {EV_KEY, KEY_A, 0}}, nullptr, "one");
    runtime.scheduler().run();
    CHECK_EQ(raw->log, std::vector<std::string>({"key B down", "key C down"}));
}

TEST_CASE(idle_grab_rejects_held_keys_and_rolls_back_races) {
    int snapshots = 0;
    int grabs = 0;
    int ungrabs = 0;
    CHECK_THROWS(grab_idle_input([&] { ++snapshots; return std::vector<int>{KEY_A}; },
                                 [] { return PendingInput{}; },
                                 [&] { ++grabs; }, [&] { ++ungrabs; }), RuntimeErrorKeyloom);
    CHECK_EQ(snapshots, 1);
    CHECK_EQ(grabs, 0);
    CHECK_EQ(ungrabs, 0);
    snapshots = 0;
    std::vector<RawEvent> queue;
    int drains = 0;
    CHECK_THROWS(grab_idle_input([&] {
        ++snapshots;
        // EVIOCGKEY would flush queued EV_KEY events while reporting no key held.
        queue.clear();
        return std::vector<int>{};
    }, [&] {
        ++drains;
        PendingInput pending;
        pending.any = !queue.empty();
        for (const auto& event : queue) {
            if (event.type == EV_KEY && event.value == 0) pending.released_codes.insert(event.code);
        }
        queue.clear();
        return pending;
    }, [&] {
        ++grabs;
        // A complete transition is invisible to a held-key snapshot.
        queue = {{EV_KEY, KEY_A, 1}, {EV_KEY, KEY_A, 0}};
    }, [&] { ++ungrabs; }), RuntimeErrorKeyloom);
    CHECK_EQ(snapshots, 2);
    CHECK_EQ(drains, 2);
    CHECK_EQ(grabs, 1);
    CHECK_EQ(ungrabs, 1);
    drains = 0;
    CHECK_THROWS(grab_idle_input([] { return std::vector<int>{}; }, [&] {
        if (++drains == 2) throw RuntimeErrorKeyloom("queue read failed");
        return PendingInput{};
    }, [&] { ++grabs; }, [&] { ++ungrabs; }), RuntimeErrorKeyloom);
    CHECK_EQ(grabs, 2);
    CHECK_EQ(ungrabs, 2);
}

TEST_CASE(split_grab_handoff_aborts_and_identifies_the_possible_stuck_key) {
    bool desktop_thinks_down = false;
    bool grabbed = false;
    int snapshots = 0;
    int drains = 0;
    std::string error_message;
    try {
        grab_idle_input([&] {
            ++snapshots;
            return std::vector<int>{};
        }, [&] {
            ++drains;
            if (drains == 1) return PendingInput{};
            // The desktop saw the physical down before the grab. The physical
            // up arrived while grabbed and cannot safely be replayed elsewhere.
            return PendingInput{true, false, {KEY_A}};
        }, [&] {
            desktop_thinks_down = true;
            grabbed = true;
        }, [&] { grabbed = false; });
    } catch (const RuntimeErrorKeyloom& error) {
        error_message = error.what();
    }
    CHECK_EQ(snapshots, 2);
    CHECK_EQ(drains, 2);
    CHECK_TRUE(!grabbed);
    CHECK_TRUE(desktop_thinks_down);
    CHECK_TRUE(error_message.find("release of A") != std::string::npos);
    CHECK_TRUE(error_message.find("physically tap and release") != std::string::npos);
}

TEST_CASE(handoff_queue_drain_preserves_releases_and_overflow) {
    int fds[2];
    CHECK_EQ(::pipe2(fds, O_NONBLOCK | O_CLOEXEC), 0);
    UniqueFd reader(fds[0]);
    UniqueFd writer(fds[1]);
    input_event events[3]{};
    events[0].type = EV_KEY;
    events[0].code = KEY_A;
    events[0].value = 1;
    events[1].type = EV_KEY;
    events[1].code = KEY_A;
    events[1].value = 0;
    events[2].type = EV_SYN;
    events[2].code = SYN_DROPPED;
    CHECK_EQ(::write(writer.get(), events, sizeof(events)), static_cast<ssize_t>(sizeof(events)));
    const PendingInput pending = drain_input_events(reader.get());
    CHECK_TRUE(pending.any);
    CHECK_TRUE(pending.synchronization_lost);
    CHECK_TRUE(pending.released_codes.count(KEY_A) == 1);
    CHECK_TRUE(!drain_input_events(reader.get()).any);
}

TEST_CASE(disconnect_releases_passthrough_before_ungrabbing) {
    RecordedInput recorder;
    PassthroughDevice device({}, [&recorder](const std::string&, const std::string&, const DeviceCapabilities&) {
        return std::make_unique<ForwardingInput>(&recorder);
    });
    device.write_event(EV_KEY, KEY_A, 1);
    device.write_event(EV_SYN, SYN_REPORT, 0);
    release_input_capture(&device, [&] { recorder.writes.push_back("ungrab"); },
                          [&] { recorder.writes.push_back("close physical"); });
    CHECK_EQ(recorder.writes, std::vector<std::string>({"1:30:1", "0:0:0", "flush", "1:30:0",
                                                        "0:0:0", "flush", "ungrab", "close physical"}));
    CHECK_EQ(recorder.closes, 1);
}

TEST_CASE(disconnect_attempts_all_steps_after_passthrough_close_failure) {
    struct BrokenSink : EventSink {
        void write_event(int, int, int) override {}
        void close() override { throw BackendError("close failed"); }
    } sink;
    std::vector<std::string> actions;
    CHECK_THROWS(release_input_capture(&sink, [&] { actions.push_back("ungrab"); },
                                       [&] { actions.push_back("close physical"); }), BackendError);
    CHECK_EQ(actions, std::vector<std::string>({"ungrab", "close physical"}));
}

TEST_CASE(chord_hold_rolls_back_after_partial_backend_failure) {
    struct FailOnKeyPress : RecordingBackend {
        std::string fail_on;
        void key(const std::string& name, bool down) override {
            if (down && name == fail_on) throw BackendError("injected key press failure");
            RecordingBackend::key(name, down);
        }
    };
    const Script script = parse(R"LOOM(
loom "chord-rollback" {
    when key F8 goes down {
        key CTRL+SHIFT tap
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<FailOnKeyPress>();
    backend->fail_on = "LEFTSHIFT";
    auto* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.physical_key("fake-keyboard", "F8", true);
    runtime.scheduler().run();

    CHECK_TRUE(runtime.runtime_error().find("injected key press failure") != std::string::npos);
    CHECK_EQ(raw->log, std::vector<std::string>({"key LEFTCTRL down", "key LEFTCTRL up"}));
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

TEST_CASE(ydotool_close_releases_every_held_input_once) {
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
    YdotoolBackend backend(1920, 1080, {}, runner, "/usr/bin/ydotool");
    backend.key("A", true);
    backend.key("B", true);
    backend.key("C", true);
    backend.button("left", true);
    backend.button("right", true);
    backend.close();
    backend.close();
    CHECK_EQ(calls, std::vector<std::string>({"/usr/bin/ydotool --help", "/usr/bin/ydotool key 30:1",
        "/usr/bin/ydotool key 48:1", "/usr/bin/ydotool key 46:1", "/usr/bin/ydotool click 0x40",
        "/usr/bin/ydotool click 0x41", "/usr/bin/ydotool click 0x80", "/usr/bin/ydotool click 0x81",
        "/usr/bin/ydotool key 30:0", "/usr/bin/ydotool key 48:0", "/usr/bin/ydotool key 46:0"}));
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

TEST_CASE(release_only_binding_runs_once_per_physical_press) {
    const Script script = parse(R"LOOM(
loom "release" {
    when key A goes up { key B press }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");
    runtime.physical_key("one", "A", true);
    runtime.physical_key("two", "A", true);
    runtime.physical_key("one", "A", false);
    runtime.scheduler().run();
    CHECK_TRUE(raw->log.empty());
    runtime.physical_key("two", "A", false);
    runtime.scheduler().run();
    CHECK_EQ(raw->log, std::vector<std::string>({"key B down"}));
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
