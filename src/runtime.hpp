#pragma once

#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ast.hpp"
#include "backends.hpp"
#include "gui.hpp"
#include "input.hpp"
#include "scheduler.hpp"

namespace keyloom {

/// Resolve aliases and upper-case a key name (`ctrl` -> `LEFTCTRL`).
std::string canonical_key(const std::string& name);

/// Best-effort virtual desktop size discovery; users can override it.
std::pair<int, int> detect_screen_size();

/// Interpret `option screen` / `--screen-size`; throws RuntimeErrorKeyloom for
/// anything that is not "auto" or WIDTHxHEIGHT.
std::pair<int, int> parse_screen_option(const Value* value, std::pair<int, int> fallback);

class Runtime {
public:
    struct Options {
        bool dry_run = false;
        std::string input_device;
        std::string output_backend;
        std::string screen_size;
        bool verbose = false;
    };

    explicit Runtime(Script script);
    Runtime(Script script, Options options);
    ~Runtime();

    /// Run until the panic key, a script error, or a signal stops the runtime.
    Task run();

    /// The exception that ended the run, if any. The scheduler swallows
    /// coroutine exceptions, so callers rethrow this after driving the loop.
    const std::exception_ptr& failure() const { return failure_; }

    /// Dispatch one event block, used by tests to exercise statements without
    /// opening devices.
    Task run_event(Node event, std::string key = std::string(),
                         std::string edge = std::string());

    // ---- seams shared with the CLI and the tests ----
    Scheduler& scheduler() { return scheduler_; }
    void set_backend(std::unique_ptr<OutputBackend> backend) { backend_ = std::move(backend); }
    void set_panic_key(std::string key) { panic_key_ = std::move(key); }
    void set_consumed_keys(std::set<std::string> keys) { consumed_keys_ = std::move(keys); }
    const std::string& panic_key() const { return panic_key_; }
    std::set<std::string>& pressed_keys() { return pressed_keys_; }
    std::map<std::string, Value>& variables() { return variables_; }
    bool interrupted() const { return interrupted_; }
    const std::string& runtime_error() const { return runtime_error_; }
    int width() const { return width_; }
    int height() const { return height_; }

    std::set<std::string> collect_consumed_keys() const;
    void hold_button(const std::string& owner, const std::string& button);
    void free_button(const std::string& owner);
    void hold_key(const std::string& name);
    void free_key(const std::string& name);
    void physical_key(const std::string& path, const std::string& key, bool down);
    void seed_device_keys(const std::string& path, const std::vector<int>& codes);
    void disconnect_device(const std::string& path);
    void process_events(const std::vector<RawEvent>& events, EventSink* sink, const std::string& path,
                        const std::function<std::vector<int>()>& snapshot = {});
    Task held_loop(Node event, std::string key, double interval);

private:
    struct EventContext {
        std::string event_kind;
        std::string key;
        std::string edge;
        /// The identifier of the binding that is running. A chord keeps all of
        /// its keys, so this is "LEFTCTRL+LEFTALT+T" for CTRL+ALT+T.
        std::string hotkey_id;
    };

    static constexpr long long kMaxLoop = 10000;
    /// How deep user-defined functions may call each other.
    static constexpr int kMaxCallDepth = 32;

    bool hotkey_enabled(const Node& event) const;

    std::unique_ptr<OutputBackend> select_backend();
    void say(const std::string& message);
    std::vector<std::unique_ptr<InputDevice>> open_input_devices(bool include_mice);
    std::string input_permission_help() const;
    std::string mouse_permission_help() const;
    bool is_consumed_key_event(int type, int code) const;
    void release_all_holds();
    void spawn_background(Task task);
    Task::handle_type spawn_held(const Node& event, const std::string& key);
    void prune_background();

    Task run_body();
    Task read_device(InputDevice* device, std::shared_ptr<PassthroughDevice> passthrough);
    Task dispatch(const Node& event, std::string key, std::string edge);
    Task safe_dispatch(const Node& event, std::string key, std::string edge);
    Task execute_nodes(const std::vector<Node>& nodes, const EventContext& context);
    Task execute(const Node& node, const EventContext& context);
    void send_string(const std::string& text);
    void send_target(const std::vector<std::string>& names, const std::vector<std::string>& active_modifiers,
                     const std::vector<std::string>& pending_modifiers, bool add_shift);
    void send_char(char c, const std::vector<std::string>& active_modifiers,
                   const std::vector<std::string>& pending_modifiers);
    Task gui_loop(GuiHost* host);
    void dispatch_gui_event(const GuiHost::Event& event);
    Task glide(int x, int y, double duration);
    Task timer_loop(const Node& event, double interval);
    Task signal_loop(int fd, std::shared_ptr<void> signal_owner);

    /// Evaluate an expression into `out`. Evaluation is a coroutine because a
    /// user-defined function may wait, so the result cannot travel back
    /// through the return value.
    Task eval(const ExprPtr& expr, const EventContext& context, Value& out);
    Value call_function(const std::string& name, std::vector<Value> args, const EventContext& context);
    Task call_user_function(const Function& function, std::vector<Value> args, const EventContext& context,
                            Value& out);
    bool chord_modifiers_held(const Node& event) const;

    /// Read a variable, preferring the innermost function scope.
    bool lookup_variable(const std::string& name, Value& out) const;
    /// Write a variable into the innermost function scope, or into the script
    /// scope when no function is running.
    void assign_variable(const std::string& name, Value value);
    std::map<std::string, std::string> active_window_info() const;
    void gui_alert(const std::string& message, const std::string& title);
    std::string gui_input(const std::string& prompt, const std::string& default_value, const std::string& title);
    bool gui_confirm(const std::string& question, const std::string& title);

    std::string clipboard(const std::string& action, const std::string& value = std::string());
    void notify_desktop(const std::string& message);
    void launch_program(const std::string& command);

    Script script_;
    Options options_;
    Scheduler scheduler_;
    std::unique_ptr<OutputBackend> backend_;
    int width_ = 1920;
    int height_ = 1080;
    std::map<std::string, Value> variables_;
    std::set<std::string> pressed_keys_;
    std::map<std::string, std::set<std::string>> device_down_;
    std::map<std::string, std::set<int>> device_raw_down_;
    std::set<std::string> desynchronized_devices_;
    std::set<std::string> consumed_keys_;
    std::vector<std::unique_ptr<InputDevice>> devices_;
    std::map<std::string, std::string> held_tokens_;
    std::map<std::string, int> button_refs_;
    std::map<std::string, int> key_refs_;
    int tap_counter_ = 0;
    std::map<std::pair<std::string, int>, Task::handle_type> held_tasks_;
    std::map<std::string, std::vector<std::string>> active_remaps_;
    bool suspended_hotkeys_ = false;
    std::set<std::string> disabled_hotkeys_;
    std::set<std::string> suspended_exempt_hotkeys_;
    std::map<std::string, std::vector<const Node*>> active_hotkey_ups_;
    std::map<std::string, const Function*> functions_;
    /// One scope per running function call. A function reads the script's
    /// variables but keeps its own assignments to itself.
    std::vector<std::map<std::string, Value>> function_scopes_;
    int loop_depth_ = 0;
    int function_depth_ = 0;
    std::vector<std::unique_ptr<GuiHost>> gui_hosts_;
    std::vector<Task::handle_type> background_;
    std::string panic_key_;
    double started_at_ = 0.0;
    std::exception_ptr failure_;
    std::string runtime_error_;
    bool interrupted_ = false;
    bool running_ = false;
    std::mt19937_64 random_engine_;
};

}  // namespace keyloom
