#include "runtime.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/input.h>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <sys/signalfd.h>
#include <unordered_map>
#include <unistd.h>

#include "builtins.hpp"
#include "errors.hpp"
#include "keys.hpp"
#include "runtime_internal.hpp"
#include "subprocess.hpp"


namespace loomwright {
namespace {

/// Evaluate an expression into `out`. An expression that cannot wait is
/// evaluated without a coroutine, so the hot path of a script does not
/// allocate a coroutine frame for every operation. Only an expression that
/// calls a user-defined function needs the awaiting evaluator, because such a
/// call may wait for the function body.
#define LOOMWRIGHT_EVAL(expression, context, out)                              \
    do {                                                                        \
        const ExprPtr& loomwright_expression_ = (expression);                   \
        if (loomwright_expression_->may_wait) {                                 \
            co_await eval(loomwright_expression_, (context), (out));            \
        } else {                                                                \
            (out) = eval_pure(*loomwright_expression_, (context));              \
        }                                                                       \
    } while (false)

using detail::lower_ascii;
using detail::split_on;
using detail::trim_ascii;

struct SignalScope {
    sigset_t previous{};
    int fd = -1;

    SignalScope() {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        if (::sigprocmask(SIG_BLOCK, &mask, &previous) < 0)
            throw RuntimeErrorLoomwright("cannot block signals: " + std::string(std::strerror(errno)));
        fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (fd < 0) {
            const int error = errno;
            ::sigprocmask(SIG_SETMASK, &previous, nullptr);
            throw RuntimeErrorLoomwright("cannot create signal fd: " + std::string(std::strerror(error)));
        }
    }
    ~SignalScope() {
        if (fd >= 0) ::close(fd);
        ::sigprocmask(SIG_SETMASK, &previous, nullptr);
    }
    SignalScope(const SignalScope&) = delete;
    SignalScope& operator=(const SignalScope&) = delete;
};

const char* type_name(const Value& value) {
    switch (value.type) {
        case Value::Type::Int: return "int";
        case Value::Type::Float: return "float";
        case Value::Type::Bool: return "bool";
        case Value::Type::Str: return "str";
        case Value::Type::List: return "list";
        case Value::Type::None: return "none";
    }
    return "object";
}

/// Control flow inside a loop body. The loop handler catches these; they
/// never reach the script-level error reporting.
struct BreakSignal : std::exception {
    const char* what() const noexcept override { return "break"; }
};

struct ContinueSignal : std::exception {
    const char* what() const noexcept override { return "continue"; }
};

/// Carries the value of a `return` statement out of a function body.
struct ReturnSignal {
    Value value = Value::none();
};

/// Runs a callable when the scope ends, whatever happens inside it.
template <typename Fn>
struct ScopeExit {
    Fn action;
    ~ScopeExit() { action(); }
};

template <typename Fn>
ScopeExit<Fn> make_scope_exit(Fn action) {
    return ScopeExit<Fn>{std::move(action)};
}

bool numeric_like(const Value& value) {
    return value.is_number() || value.type == Value::Type::Bool;
}

std::string join_sorted(const std::set<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += ", ";
        out += value;
    }
    return out;
}

int compare_values(const Value& left, const Value& right) {
    if (numeric_like(left) && numeric_like(right)) {
        if (left.type != Value::Type::Float && right.type != Value::Type::Float) {
            const long long a = left.as_int();
            const long long b = right.as_int();
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        const double a = left.as_double();
        const double b = right.as_double();
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    if (left.type == Value::Type::Str && right.type == Value::Type::Str) {
        const int result = left.text.compare(right.text);
        return result < 0 ? -1 : (result > 0 ? 1 : 0);
    }
    throw RuntimeErrorLoomwright(std::string("'<' not supported between instances of '") + type_name(left) +
                              "' and '" + type_name(right) + "'");
}

/// The spelling of a binary operator, for the error messages.
const char* binary_op_text(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Subtract: return "-";
        case BinaryOp::Multiply: return "*";
        case BinaryOp::Divide: return "/";
        case BinaryOp::Modulo: return "%";
        case BinaryOp::Equal: return "==";
        case BinaryOp::NotEqual: return "!=";
        case BinaryOp::Less: return "<";
        case BinaryOp::LessEqual: return "<=";
        case BinaryOp::Greater: return ">";
        case BinaryOp::GreaterEqual: return ">=";
        case BinaryOp::And: return "and";
        case BinaryOp::Or: return "or";
    }
    return "?";
}

/// The result of a unary operator on an evaluated operand.
Value apply_unary(UnaryOp op, const Value& value) {
    switch (op) {
        case UnaryOp::Negate:
            if (value.type == Value::Type::Float) return Value::of_float(-value.real);
            return Value::of_int(-value.as_int());
        case UnaryOp::Plus:
            return value;
        case UnaryOp::Not:
            return Value::of_bool(!value.truthy());
    }
    return Value::none();
}

/// The result of a binary operator on two evaluated operands. The operands
/// are already evaluated, so the operator itself never waits.
Value apply_binary(BinaryOp op, const Value& left, const Value& right) {
    switch (op) {
        case BinaryOp::Equal:
            return Value::of_bool(value_equals(left, right));
        case BinaryOp::NotEqual:
            return Value::of_bool(!value_equals(left, right));
        case BinaryOp::Less:
        case BinaryOp::LessEqual:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEqual: {
            const int comparison = compare_values(left, right);
            switch (op) {
                case BinaryOp::Less: return Value::of_bool(comparison < 0);
                case BinaryOp::LessEqual: return Value::of_bool(comparison <= 0);
                case BinaryOp::Greater: return Value::of_bool(comparison > 0);
                default: return Value::of_bool(comparison >= 0);
            }
        }
        case BinaryOp::Add: {
            if (left.type == Value::Type::Str && right.type == Value::Type::Str) {
                return Value::of_str(left.text + right.text);
            }
            if (left.is_list() && right.is_list()) {
                std::vector<Value> joined = left.list_items();
                for (const Value& item : right.list_items()) joined.push_back(item);
                return Value::of_list(std::move(joined));
            }
            break;
        }
        default:
            break;
    }
    if (!numeric_like(left) || !numeric_like(right)) [[unlikely]] {
        throw RuntimeErrorLoomwright("unsupported operand type(s) for " + std::string(binary_op_text(op)) +
                                     ": '" + type_name(left) + "' and '" + type_name(right) + "'");
    }
    const bool want_float = left.type == Value::Type::Float || right.type == Value::Type::Float;
    switch (op) {
        case BinaryOp::Divide: {
            const double divisor = right.as_double();
            if (divisor == 0.0) throw RuntimeErrorLoomwright("Division by zero");
            return Value::of_float(left.as_double() / divisor);
        }
        case BinaryOp::Modulo: {
            if (!want_float) {
                const long long divisor = right.as_int();
                if (divisor == 0) throw RuntimeErrorLoomwright("Division by zero");
                const long long value = left.as_int();
                long long result = value % divisor;
                if (result != 0 && ((result < 0) != (divisor < 0))) result += divisor;
                return Value::of_int(result);
            }
            const double divisor = right.as_double();
            if (divisor == 0.0) throw RuntimeErrorLoomwright("Division by zero");
            double result = std::fmod(left.as_double(), divisor);
            if (result != 0.0 && ((result < 0.0) != (divisor < 0.0))) result += divisor;
            return Value::of_float(result);
        }
        case BinaryOp::Add:
            return want_float ? Value::of_float(left.as_double() + right.as_double())
                              : Value::of_int(left.as_int() + right.as_int());
        case BinaryOp::Subtract:
            return want_float ? Value::of_float(left.as_double() - right.as_double())
                              : Value::of_int(left.as_int() - right.as_int());
        case BinaryOp::Multiply:
            return want_float ? Value::of_float(left.as_double() * right.as_double())
                              : Value::of_int(left.as_int() * right.as_int());
        default:
            break;
    }
    throw RuntimeErrorLoomwright("unsupported operator " + quote_string(binary_op_text(op)));
}

size_t utf8_length(const std::string& text) {
    size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

/// Byte offset of the character at `index`; clamped to the end of the text.
size_t utf8_offset(const std::string& text, size_t index) {
    size_t offset = 0;
    size_t count = 0;
    while (offset < text.size() && count < index) {
        ++offset;
        while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80) ++offset;
        ++count;
    }
    return offset;
}

/// Releases a mouse hold token when a tap or statement scope ends, including
/// when the task is cancelled mid-tap.
struct ButtonRelease {
    Runtime* runtime;
    std::string owner;
    ~ButtonRelease() {
        try { runtime->free_button(owner); } catch (...) {}
    }
};

struct KeyRelease {
    Runtime* runtime;
    std::vector<std::string> names;
    ~KeyRelease() {
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            try { runtime->free_key(*it); } catch (...) {}
        }
    }
};

/// The identifier of a binding. A chord keeps every key of the chord, so
/// `set_hotkey_enabled("CTRL+ALT+T", false)` addresses the whole binding.
std::string hotkey_id_for_event(const Node& event) {
    if (event.event_kind == "key") {
        if (!event.chord_parts.empty()) {
            std::string id;
            for (size_t index = 0; index < event.chord_parts.size(); ++index) {
                if (index > 0) id += "+";
                id += canonical_key(event.chord_parts[index]);
            }
            return id;
        }
        return canonical_key(event.event_key);
    }
    if (event.event_kind == "mouse") return canonical_mouse_event_key(event.event_key);
    return std::string();
}

std::string hotkey_id_for_name(const std::string& raw_name) {
    const std::string name = trim_ascii(raw_name);
    const std::string lower = lower_ascii(name);
    if (lower.starts_with("mouse:")) return canonical_mouse_event_key(name.substr(6));
    if (lower.starts_with("mouse_")) return canonical_mouse_event_key(name);
    if (name.find('+') != std::string::npos) {
        std::string id;
        size_t start = 0;
        while (true) {
            const size_t plus = name.find('+', start);
            const std::string part =
                plus == std::string::npos ? name.substr(start) : name.substr(start, plus - start);
            if (!id.empty()) id += "+";
            id += canonical_key(trim_ascii(part));
            if (plus == std::string::npos) break;
            start = plus + 1;
        }
        return id;
    }
    return canonical_key(name);
}

}  // namespace


std::string canonical_key(const std::string& name) { return canonical_key_name(name); }

std::pair<int, int> detect_screen_size() {
    const char* env_width = std::getenv("LOOMWRIGHT_SCREEN_WIDTH");
    const char* env_height = std::getenv("LOOMWRIGHT_SCREEN_HEIGHT");
    if (env_width != nullptr && env_height != nullptr) {
        try {
            return {std::max(1, std::stoi(env_width)), std::max(1, std::stoi(env_height))};
        } catch (const std::exception&) {
        }
    }

    // wlr-randr covers wlroots-based Wayland compositors.
    const std::string wlr = which("wlr-randr");
    if (!wlr.empty()) {
        const CommandResult result = run_command({wlr}, std::string(), 2000);
        if (result.returncode == 0) {
            static const std::regex pattern(R"((\d+)x(\d+)\s+px)");
            int width = 0;
            int height = 0;
            std::string line;
            std::istringstream stream(result.out);
            while (std::getline(stream, line)) {
                if (line.find(" px") == std::string::npos) continue;
                std::smatch match;
                if (std::regex_search(line, match, pattern)) {
                    width = std::max(width, std::stoi(match[1].str()));
                    height = std::max(height, std::stoi(match[2].str()));
                }
            }
            if (width > 0 && height > 0) return {width, height};
        }
    }

    const std::string xrandr = which("xrandr");
    if (!xrandr.empty()) {
        const CommandResult result = run_command({xrandr, "--current"}, std::string(), 2000);
        if (result.returncode == 0) {
            static const std::regex pattern(R"((\d+)x(\d+)\+(-?\d+)\+(-?\d+))");
            bool found = false;
            int min_x = 0;
            int min_y = 0;
            int max_x = 0;
            int max_y = 0;
            std::istringstream stream(result.out);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.find(" connected") == std::string::npos) continue;
                std::smatch match;
                if (!std::regex_search(line, match, pattern)) continue;
                const int width = std::stoi(match[1].str());
                const int height = std::stoi(match[2].str());
                const int x = std::stoi(match[3].str());
                const int y = std::stoi(match[4].str());
                if (!found) {
                    min_x = x;
                    min_y = y;
                    max_x = x + width;
                    max_y = y + height;
                    found = true;
                } else {
                    min_x = std::min(min_x, x);
                    min_y = std::min(min_y, y);
                    max_x = std::max(max_x, x + width);
                    max_y = std::max(max_y, y + height);
                }
            }
            if (found) {
                return {std::max(1, max_x - std::min(0, min_x)), std::max(1, max_y - std::min(0, min_y))};
            }
        }
    }

    const std::string xdpyinfo = which("xdpyinfo");
    if (!xdpyinfo.empty()) {
        const CommandResult result = run_command({xdpyinfo}, std::string(), 2000);
        if (result.returncode == 0) {
            static const std::regex pattern(R"(dimensions:\s+(\d+)x(\d+) pixels)");
            std::smatch match;
            if (std::regex_search(result.out, match, pattern)) {
                return {std::stoi(match[1].str()), std::stoi(match[2].str())};
            }
        }
    }
    return {1920, 1080};
}

std::pair<int, int> parse_screen_option(const Value* value, std::pair<int, int> fallback) {
    if (value == nullptr || value->is_none()) return fallback;
    if (value->type != Value::Type::Str) {
        throw RuntimeErrorLoomwright("option screen must be a string such as '1920x1080'");
    }
    const std::string text = lower_ascii(value->text);
    if (text == "auto") return fallback;
    const size_t separator = text.find('x');
    if (separator == std::string::npos) {
        throw RuntimeErrorLoomwright("option screen must look like '1920x1080' or 'auto'");
    }
    try {
        const std::string width_text = text.substr(0, separator);
        const std::string height_text = text.substr(separator + 1);
        size_t used = 0;
        const int width = std::stoi(width_text, &used);
        if (used != width_text.size() || width_text.empty()) throw std::invalid_argument("width");
        const int height = std::stoi(height_text, &used);
        if (used != height_text.size() || height_text.empty()) throw std::invalid_argument("height");
        return {std::max(1, width), std::max(1, height)};
    } catch (const std::exception&) {
        throw RuntimeErrorLoomwright("option screen must look like '1920x1080' or 'auto'");
    }
}

bool Runtime::hotkey_enabled(const Node& event) const {
    if (event.event_kind != "key" && event.event_kind != "mouse") return true;
    const std::string id = hotkey_id_for_event(event);
    if (disabled_hotkeys_.find(id) != disabled_hotkeys_.end()) return false;
    if (suspended_hotkeys_ && suspended_exempt_hotkeys_.find(id) == suspended_exempt_hotkeys_.end()) {
        return false;
    }
    return true;
}

bool Runtime::chord_modifiers_held(const Node& event) const {
    if (event.chord_parts.size() < 2) return true;
    for (size_t index = 0; index + 1 < event.chord_parts.size(); ++index) {
        const std::string modifier = canonical_key(event.chord_parts[index]);
        if (pressed_keys_.find(modifier) == pressed_keys_.end()) return false;
    }
    return true;
}

Runtime::Runtime(Script script) : Runtime(std::move(script), Options{}) {}

Runtime::Runtime(Script script, Options options)
    : script_(std::move(script)), options_(std::move(options)) {
    const auto detected = detect_screen_size();
    if (!options_.screen_size.empty()) {
        const Value override_value = Value::of_str(options_.screen_size);
        const auto parsed = parse_screen_option(&override_value, detected);
        width_ = parsed.first;
        height_ = parsed.second;
    } else {
        const auto parsed = parse_screen_option(script_.option("screen"), detected);
        width_ = parsed.first;
        height_ = parsed.second;
    }

    variables_["screen_width"] = Value::of_int(width_);
    variables_["screen_height"] = Value::of_int(height_);
    variables_["pi"] = Value::of_float(3.14159265358979323846);
    variables_["e"] = Value::of_float(2.71828182845904523536);
    started_at_ = scheduler_.now();

    for (const auto& function : script_.functions) functions_.emplace(function.name, &function);

    std::random_device device;
    const auto seed = static_cast<std::uint64_t>(device()) ^
                      static_cast<std::uint64_t>(started_at_ * 1'000'000.0) ^
                      static_cast<std::uint64_t>(::getpid());
    random_engine_.seed(seed);
}

Runtime::~Runtime() {
    scheduler_.shutdown();
    release_all_holds();
    if (backend_) {
        try {
            backend_->close();
        } catch (const std::exception&) {
        }
    }
}

void Runtime::say(const std::string& message) { std::cout << "[loomwright] " << message << '\n' << std::flush; }

std::unique_ptr<OutputBackend> Runtime::select_backend() {
    if (options_.dry_run) return std::make_unique<DryRunBackend>();
    std::string name = options_.output_backend;
    if (name.empty()) {
        const Value* option = script_.option("output");
        name = option != nullptr ? option->to_string() : std::string("auto");
    }
    return choose_backend(name, width_, height_);
}

void Runtime::spawn_background(Task task) {
    if (!task.handle) return;
    const Task::handle_type handle = task.handle;
    scheduler_.spawn(std::move(task));
    background_.push_back(handle);
    prune_background();
}

void Runtime::prune_background() {
    if (background_.size() < 256) return;
    background_.erase(std::remove_if(background_.begin(), background_.end(),
                                     [this](Task::handle_type handle) { return !scheduler_.is_live(handle); }),
                      background_.end());
}

Task::handle_type Runtime::spawn_held(const Node& event, const std::string& key) {
    Task task = held_loop(event, key, event.interval);
    const Task::handle_type handle = task.handle;
    scheduler_.spawn(std::move(task));
    background_.push_back(handle);
    prune_background();
    return handle;
}

std::string Runtime::input_permission_help() const {
    return "No accessible keyboard devices were found. Loomwright reads /dev/input/event* through evdev. "
           "On most distributions, add your user to the 'input' group (then log out/in), or create a "
           "narrow udev rule for your keyboard. Do not run the whole automation as root. "
           "Use `loomwright --devices` to inspect permissions.";
}

std::string Runtime::mouse_permission_help() const {
    return "No accessible mouse devices were found. Loomwright reads /dev/input/event* through evdev and "
           "needs a device that reports both BTN_LEFT and BTN_RIGHT. On most distributions, add your "
           "user to the 'input' group (then log out/in), or create a narrow udev rule for your mouse. "
           "Do not run the whole automation as root. Use `loomwright --devices` to inspect permissions.";
}

std::set<std::string> Runtime::collect_consumed_keys() const {
    std::set<std::string> found;
    std::vector<const Node*> stack;
    for (const auto& event : script_.events) {
        stack.push_back(&event);
        while (!stack.empty()) {
            const Node* node = stack.back();
            stack.pop_back();
            if (node->stmt == StmtKind::Consume && !node->is_event) found.insert(canonical_key(node->a));
            for (const auto& child : node->body) stack.push_back(&child);
            for (const auto& child : node->otherwise) stack.push_back(&child);
        }
    }
    for (const auto& remap : script_.remaps) found.insert(canonical_key(remap.source));
    if (!panic_key_.empty()) found.erase(panic_key_);
    return found;
}

bool Runtime::is_consumed_key_event(int type, int code) const {
    if (type != EV_KEY) return false;
    const auto name = normalize_key_event(code);
    if (!name) return false;
    const std::string key = canonical_key(*name);
    if (!panic_key_.empty() && key == panic_key_) return false;
    return consumed_keys_.find(key) != consumed_keys_.end();
}

Task Runtime::signal_loop(int fd, std::shared_ptr<void> signal_owner) {
    // Retain the fd and its signal mask for the entire task, even if its
    // spawning coroutine finishes unexpectedly.
    (void)signal_owner;
    while (true) {
        co_await readable(fd);
        signalfd_siginfo info{};
        const ssize_t count = ::read(fd, &info, sizeof(info));
        if (count != static_cast<ssize_t>(sizeof(info))) continue;
        if (info.ssi_signo == SIGINT) interrupted_ = true;
        scheduler_.request_stop();
    }
}

Task Runtime::run() {
    try {
        co_await run_body();
    } catch (...) {
        failure_ = std::current_exception();
        throw;
    }
}

Task Runtime::run_event(Node event, std::string key, std::string edge) {
    // Goes through safe_dispatch so a failing event behaves exactly as it does
    // when a real binding fires: the error is reported and the run stops.
    co_await safe_dispatch(event, std::move(key), std::move(edge));
}

Task Runtime::run_body() {
    std::exception_ptr failure;
    std::vector<std::shared_ptr<PassthroughDevice>> passthroughs;
    std::shared_ptr<SignalScope> signals;
    try {
        if (::access("/proc", F_OK) != 0) {
            throw RuntimeErrorLoomwright("Loomwright's live runtime is Linux-only.");
        }
        const Value* compositor_option = script_.option("compositor");
        std::string compositor = compositor_option ? compositor_option->to_string() : "auto";
        const bool automatic_compositor = compositor == "auto";
        if (compositor != "auto") validate_compositor(compositor);
        else if (!script_.panels.empty()) compositor = resolve_compositor();
        // GTK may create helper threads during panel startup. Block signals
        // first so those threads inherit the mask and signalfd handles stop.
        signals = std::make_shared<SignalScope>();
        spawn_background(signal_loop(signals->fd, signals));
        if (!backend_) backend_ = select_backend();
        running_ = true;

        std::string panic_text = "PAUSE";
        if (const Value* panic = script_.option("panic")) panic_text = panic->to_string();
        panic_key_ = lower_ascii(panic_text) == "none" ? std::string() : canonical_key(panic_text);
        consumed_keys_ = collect_consumed_keys();

        for (const auto& panel : script_.panels) {
            std::unique_ptr<GuiHost> host;
            try {
                host = GuiHost::create(panel, compositor);
            } catch (const RuntimeErrorLoomwright& error) {
                if (!automatic_compositor || compositor != "wayland" || !gui_hosts_.empty()) throw;
                const std::string native_error = error.what();
                compositor = "x11";
                try {
                    host = GuiHost::create(panel, compositor);
                } catch (const RuntimeErrorLoomwright& fallback_error) {
                    throw RuntimeErrorLoomwright("Could not start a native Wayland panel (" + native_error +
                                               ") or an X11 fallback (" + fallback_error.what() + ").");
                }
            }
            host->set_event_handler([this](const GuiHost::Event& event) { dispatch_gui_event(event); });
            gui_hosts_.push_back(std::move(host));
        }
        for (auto& host : gui_hosts_) spawn_background(gui_loop(host.get()));

        bool has_key_events = !script_.remaps.empty() || !consumed_keys_.empty();
        bool has_mouse_events = false;
        for (const auto& event : script_.events) {
            if (event.event_kind == "key") has_key_events = true;
            if (event.event_kind == "mouse") has_mouse_events = true;
            if (event.event_kind == "app") {
                co_await safe_dispatch(event, std::string(), "starts");
            } else if (event.event_kind == "timer") {
                spawn_background(timer_loop(event, event.interval));
            }
        }
        if (!has_key_events && !has_mouse_events) {
            say("No key bindings found; timer/app events are active. Press the panic key or Ctrl+C to stop.");
        }

        devices_ = open_input_devices(has_mouse_events);
        const auto has_device = [this](const auto& predicate) {
            return std::any_of(devices_.begin(), devices_.end(), predicate);
        };
        if (has_key_events && !has_device([](const auto& device) { return device->is_keyboard(); })) {
            throw RuntimeErrorLoomwright(input_permission_help());
        }
        if (has_mouse_events && !has_device([](const auto& device) { return device->is_mouse(); })) {
            throw RuntimeErrorLoomwright(mouse_permission_help());
        }

        if (!devices_.empty()) {
            std::map<std::string, std::shared_ptr<PassthroughDevice>> passthrough_by_path;
            if (!consumed_keys_.empty()) {
                const std::string keys = join_sorted(consumed_keys_);
                if (options_.dry_run) {
                    say("Dry run: would consume key(s) " + keys + "; devices are not grabbed.");
                } else {
                    // Only keyboards are grabbed. Mouse buttons are observed in
                    // coexistence mode: the physical click still reaches apps.
                    for (auto& device : devices_) {
                        if (!device->is_keyboard()) continue;
                        auto passthrough = std::make_shared<PassthroughDevice>(device->capabilities());
                        try {
                            grab_idle_input([&device] { return device->pressed_key_codes(); },
                                            [&device] { return device->discard_pending_events(); },
                                            [&device] { device->grab(); }, [&device] { device->ungrab(); });
                        } catch (const std::exception& error) {
                            throw RuntimeErrorLoomwright(
                                "Cannot grab input device " + device->path() + " (" + device->name() +
                                ") for key consumption; another program may already be grabbing it or your "
                                "user lacks device access. Details: " +
                                std::string(error.what()));
                        }
                        passthrough_by_path[device->path()] = passthrough;
                        passthroughs.push_back(std::move(passthrough));
                    }
                    std::string note;
                    if (!panic_key_.empty()) note = "; panic key " + panic_key_ + " still passes through";
                    say("Consuming key(s) " + keys + note + ".");
                }
            }

            for (auto& device : devices_) {
                const auto found = passthrough_by_path.find(device->path());
                auto sink = found == passthrough_by_path.end() ? nullptr : found->second;
                // EVIOCGKEY drops queued key events. Do this only for observed
                // devices; grabbed devices were checked for queued events and
                // must retain any subsequent events for passthrough.
                if (!sink) seed_device_keys(device->path(), device->pressed_key_codes());
                spawn_background(read_device(device.get(), std::move(sink)));
            }

            std::string names;
            for (const auto& device : devices_) {
                if (!names.empty()) names += ", ";
                names += std::string(device->is_mouse() ? "mouse " : "keyboard ") + device->name() + " (" +
                         device->path() + ")";
            }
            say("Listening on: " + names);
        }

        say("Running '" + script_.name + "' — panic key: " + (panic_key_.empty() ? "disabled" : panic_text));
        if (options_.verbose) {
            say(std::string("Output backend: ") + backend_->name() + "; screen bounds: " +
                std::to_string(width_) + "x" + std::to_string(height_));
        }

        co_await wait_stop();
        if (!runtime_error_.empty()) throw RuntimeErrorLoomwright(runtime_error_);
    } catch (...) {
        failure = std::current_exception();
    }

    for (const auto& handle : background_) scheduler_.cancel(handle);
    for (const auto& entry : held_tasks_) scheduler_.cancel(entry.second);
    while (!(co_await cleanup_drain())) {
        // Cancellation can wake the cleanup waiter before other tasks finish.
    }

    const auto cleanup = [&failure](const auto& operation) {
        try {
            operation();
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
    };
    release_all_holds();
    if (backend_) cleanup([this] { backend_->close(); });
    for (auto& passthrough : passthroughs) cleanup([&passthrough] { passthrough->close(); });
    passthroughs.clear();
    for (auto& device : devices_) cleanup([&device] { device->close(); });
    devices_.clear();
    gui_hosts_.clear();
    held_tasks_.clear();
    background_.clear();
    signals.reset();
    running_ = false;

    say(failure ? "Stopped; cleanup attempted." : "Stopped; held buttons and keys released.");
    if (failure) std::rethrow_exception(failure);
}

Task Runtime::timer_loop(const Node& event, double interval) {
    if (interval <= 0.0) co_return;
    while (!scheduler_.stop_requested()) {
        co_await sleep_for(interval);
        if (!scheduler_.stop_requested()) co_await safe_dispatch(event, std::string(), "tick");
    }
}

Task Runtime::held_loop(Node event, std::string key, double interval) {
    if (interval <= 0.0) co_return;
    struct HeldTaskGuard {
        Runtime* runtime;
        std::pair<std::string, int> id;
        ~HeldTaskGuard() { runtime->held_tasks_.erase(id); }
    } guard{this, {key, event.line}};
    // Tick on a fixed schedule: time spent inside the handler is part of the
    // interval, so `every 10ms` with a 5ms tap still repeats every 10ms.
    double deadline = scheduler_.now() + interval;
    while (pressed_keys_.find(key) != pressed_keys_.end() && !scheduler_.stop_requested()) {
        const double delay = deadline - scheduler_.now();
        if (delay > 0.0) co_await sleep_for(delay);
        if (pressed_keys_.find(key) == pressed_keys_.end() || scheduler_.stop_requested()) co_return;
        co_await safe_dispatch(event, key, "held");
        deadline += interval;
        const double now = scheduler_.now();
        if (deadline <= now) {
            // The handler overran its tick: skip whole ticks instead of firing a
            // burst of catch-up ticks.
            deadline += std::ceil((now - deadline) / interval) * interval;
        }
    }
}

Task Runtime::safe_dispatch(const Node& event, std::string key, std::string edge) {
    try {
        co_await dispatch(event, std::move(key), std::move(edge));
    } catch (const TaskCancelled&) {
        throw;
    } catch (const std::exception& error) {
        runtime_error_ = "script error at line " + std::to_string(event.line) + ": " + error.what();
        say(runtime_error_);
        scheduler_.request_stop();
    } catch (...) {
        runtime_error_ = "script error at line " + std::to_string(event.line) + ": unknown error";
        say(runtime_error_);
        scheduler_.request_stop();
    }
}

Task Runtime::dispatch(const Node& event, std::string key, std::string edge) {
    const EventContext context{event.event_kind, std::move(key), std::move(edge), hotkey_id_for_event(event)};
    co_await execute_nodes(event.body, context);
}

Task Runtime::execute_nodes(const std::vector<Node>& nodes, const EventContext& context) {
    for (const auto& node : nodes) {
        if (scheduler_.stop_requested()) [[unlikely]] co_return;
        if (node.may_suspend) {
            co_await execute(node, context);
        } else {
            // A statement that cannot wait runs without a coroutine frame.
            execute_pure(node, context);
        }
    }
}

void Runtime::execute_nodes_pure(const std::vector<Node>& nodes, const EventContext& context) {
    for (const auto& node : nodes) {
        if (scheduler_.stop_requested()) [[unlikely]] return;
        execute_pure(node, context);
    }
}

std::string Runtime::mouse_owner(const Node& node, const EventContext& context) const {
    if (!node.c.empty()) return node.c;
    return "default:" + (context.key.empty() ? std::string("script") : context.key) + ":" + node.a;
}

std::vector<std::string> Runtime::key_names(const Node& node) const {
    std::vector<std::string> names;
    for (const auto& part : split_on(node.a, '+')) names.push_back(canonical_key(part));
    return names;
}

/// The action of a statement whose expressions are already evaluated. `first`
/// and `second` hold the first and the second expression of the statement; a
/// statement with fewer expressions ignores the extra ones.
void Runtime::run_action(const Node& node, const Value& first, const Value& second,
                         const EventContext& context) {
    switch (node.stmt) {
        case StmtKind::Cursor: {
            if (node.a == "warp") {
                const int x = static_cast<int>(first.as_int());
                const int y = static_cast<int>(second.as_int());
                backend_->warp(x, y);
                variables_["cursor_x"] = Value::of_int(x);
                variables_["cursor_y"] = Value::of_int(y);
            } else if (node.a == "move") {
                const int dx = static_cast<int>(first.as_int());
                const int dy = static_cast<int>(second.as_int());
                backend_->move(dx, dy);
                const auto current_x = variables_.find("cursor_x");
                const auto current_y = variables_.find("cursor_y");
                const long long start_x = current_x != variables_.end() ? current_x->second.as_int() : width_ / 2;
                const long long start_y = current_y != variables_.end() ? current_y->second.as_int() : height_ / 2;
                variables_["cursor_x"] = Value::of_int(start_x + dx);
                variables_["cursor_y"] = Value::of_int(start_y + dy);
            } else if (node.a == "center") {
                backend_->warp(width_ / 2, height_ / 2);
                variables_["cursor_x"] = Value::of_int(width_ / 2);
                variables_["cursor_y"] = Value::of_int(height_ / 2);
            }
            break;
        }
        case StmtKind::Mouse: {
            const std::string& button = node.a;
            const std::string owner = mouse_owner(node, context);
            if (node.b == "hold") {
                hold_button(owner, button);
            } else if (node.b == "free") {
                free_button(owner);
            }
            break;
        }
        case StmtKind::Key: {
            const std::vector<std::string> names = key_names(node);
            if (node.b == "press") {
                std::vector<std::string> held;
                try {
                    for (const auto& name : names) { hold_key(name); held.push_back(name); }
                } catch (...) {
                    for (auto it = held.rbegin(); it != held.rend(); ++it) {
                        try { free_key(*it); } catch (...) {}
                    }
                    throw;
                }
            } else if (node.b == "free") {
                std::exception_ptr failure;
                for (auto it = names.rbegin(); it != names.rend(); ++it) {
                    try { free_key(*it); } catch (...) { if (!failure) failure = std::current_exception(); }
                }
                if (failure) std::rethrow_exception(failure);
            }
            break;
        }
        case StmtKind::Consume:
            // Consumption is collected before devices are grabbed so the first
            // key-down is swallowed; the declaration has no per-event effect.
            break;
        case StmtKind::Type:
            backend_->type_text(first.to_string());
            break;
        case StmtKind::Scroll: {
            const int x = static_cast<int>(first.as_int());
            const int y = static_cast<int>(second.as_int());
            backend_->scroll(x, y);
            break;
        }
        case StmtKind::Log:
            std::cout << first.to_string() << '\n' << std::flush;
            break;
        case StmtKind::Notify:
            notify_desktop(first.to_string());
            break;
        case StmtKind::ClipWrite:
            clipboard("write", first.to_string());
            break;
        case StmtKind::ClipRead:
            assign_variable(node.a, Value::of_str(clipboard("read")));
            break;
        case StmtKind::Launch:
            launch_program(first.to_string());
            break;
        case StmtKind::Set:
            assign_variable(node.a, first);
            break;
        case StmtKind::Break:
            if (loop_depth_ == 0) throw RuntimeErrorLoomwright("break outside a loop");
            throw BreakSignal{};
        case StmtKind::Continue:
            if (loop_depth_ == 0) throw RuntimeErrorLoomwright("continue outside a loop");
            throw ContinueSignal{};
        case StmtKind::Return: {
            if (function_depth_ == 0) throw RuntimeErrorLoomwright("return outside a function");
            ReturnSignal signal;
            if (!node.exprs.empty()) signal.value = first;
            throw signal;
        }
        case StmtKind::Assert:
            if (!first.truthy()) {
                throw RuntimeErrorLoomwright("assertion failed at line " + std::to_string(node.line));
            }
            break;
        case StmtKind::Send:
            send_string(first.to_string());
            break;
        case StmtKind::Call:
            // The result of a `call` statement is not used.
            break;
        case StmtKind::Wait:
        case StmtKind::If:
        case StmtKind::Repeat:
        case StmtKind::While:
        case StmtKind::For:
            // These statements wait or run a block; they never reach the
            // action above.
            break;
    }
}

/// Run a statement that cannot wait. No coroutine is involved, so the
/// statement runs in the caller's stack frame.
void Runtime::execute_pure(const Node& node, const EventContext& context) {
    switch (node.stmt) {
        case StmtKind::If: {
            const Value condition = eval_pure(*node.exprs.at(0), context);
            execute_nodes_pure(condition.truthy() ? node.body : node.otherwise, context);
            break;
        }
        case StmtKind::Repeat: {
            const Value count_value = eval_pure(*node.exprs.at(0), context);
            const long long count = count_value.as_int();
            if (count < 0 || count > kMaxLoop) {
                throw RuntimeErrorLoomwright("repeat count must be from 0 to " + std::to_string(kMaxLoop));
            }
            ++loop_depth_;
            const auto depth = make_scope_exit([this] { --loop_depth_; });
            for (long long index = 0; index < count; ++index) {
                if (scheduler_.stop_requested()) break;
                try {
                    execute_nodes_pure(node.body, context);
                } catch (const BreakSignal&) {
                    break;
                } catch (const ContinueSignal&) {
                    continue;
                }
            }
            break;
        }
        case StmtKind::While: {
            long long iterations = 0;
            ++loop_depth_;
            const auto depth = make_scope_exit([this] { --loop_depth_; });
            Value condition;
            while (true) {
                condition = eval_pure(*node.exprs.at(0), context);
                if (!condition.truthy()) break;
                if (scheduler_.stop_requested()) break;
                ++iterations;
                if (iterations > kMaxLoop) {
                    throw RuntimeErrorLoomwright("while loop exceeded safety limit (" + std::to_string(kMaxLoop) +
                                                 " iterations)");
                }
                try {
                    execute_nodes_pure(node.body, context);
                } catch (const BreakSignal&) {
                    break;
                } catch (const ContinueSignal&) {
                    continue;
                }
            }
            break;
        }
        case StmtKind::For: {
            ++loop_depth_;
            const auto depth = make_scope_exit([this] { --loop_depth_; });
            if (node.b == "in") {
                const Value items = eval_pure(*node.exprs.at(0), context);
                if (!items.is_list()) {
                    throw RuntimeErrorLoomwright(std::string("for ... in requires a list, got ") + type_name(items));
                }
                // Iterate a snapshot so the body may change the list.
                const std::vector<Value> snapshot = items.list_items();
                long long iterations = 0;
                for (const Value& item : snapshot) {
                    if (scheduler_.stop_requested()) break;
                    if (++iterations > kMaxLoop) {
                        throw RuntimeErrorLoomwright("for loop exceeded safety limit (" + std::to_string(kMaxLoop) +
                                                      " iterations)");
                    }
                    assign_variable(node.a, item);
                    try {
                        execute_nodes_pure(node.body, context);
                    } catch (const BreakSignal&) {
                        break;
                    } catch (const ContinueSignal&) {
                        continue;
                    }
                }
            } else {
                const Value start_value = eval_pure(*node.exprs.at(0), context);
                const Value end_value = eval_pure(*node.exprs.at(1), context);
                const long long start = start_value.as_int();
                const long long end = end_value.as_int();
                long long step = node.b == "downto" ? -1 : 1;
                if (node.exprs.size() > 2) {
                    const Value step_value = eval_pure(*node.exprs.at(2), context);
                    step = step_value.as_int();
                }
                if (step == 0) throw RuntimeErrorLoomwright("for loop step must not be zero");
                long long iterations = 0;
                for (long long value = start; step > 0 ? value <= end : value >= end; value += step) {
                    if (scheduler_.stop_requested()) break;
                    if (++iterations > kMaxLoop) {
                        throw RuntimeErrorLoomwright("for loop exceeded safety limit (" + std::to_string(kMaxLoop) +
                                                      " iterations)");
                    }
                    assign_variable(node.a, Value::of_int(value));
                    try {
                        execute_nodes_pure(node.body, context);
                    } catch (const BreakSignal&) {
                        break;
                    } catch (const ContinueSignal&) {
                        continue;
                    }
                }
            }
            break;
        }
        default: {
            // A statement whose expressions cannot wait.
            Value first;
            Value second;
            if (!node.exprs.empty()) first = eval_pure(*node.exprs.at(0), context);
            if (node.exprs.size() > 1) second = eval_pure(*node.exprs.at(1), context);
            run_action(node, first, second, context);
            break;
        }
    }
}

Task Runtime::execute(const Node& node, const EventContext& context) {
    switch (node.stmt) {
        case StmtKind::Wait: {
            Value seconds_value;
            if (node.exprs.empty()) {
                seconds_value = Value::of_float(node.duration);
            } else {
                LOOMWRIGHT_EVAL(node.exprs.at(0), context, seconds_value);
            }
            const double seconds = seconds_value.as_double();
            if (!std::isfinite(seconds) || seconds < 0) {
                throw RuntimeErrorLoomwright("wait duration must be finite and non-negative");
            }
            co_await sleep_for(seconds);
            break;
        }
        case StmtKind::Mouse: {
            if (node.b != "tap") {
                execute_pure(node, context);
                break;
            }
            const std::string& button = node.a;
            const std::string owner = mouse_owner(node, context);
            ++tap_counter_;
            const std::string tap_owner = "__loomwright_tap_" + std::to_string(tap_counter_);
            hold_button(tap_owner, button);
            ButtonRelease release{this, tap_owner};
            co_await sleep_for(node.has_duration ? node.duration : 0.06);
            break;
        }
        case StmtKind::Key: {
            if (node.b != "tap") {
                execute_pure(node, context);
                break;
            }
            const std::vector<std::string> names = key_names(node);
            std::vector<std::string> held;
            try {
                for (const auto& name : names) { hold_key(name); held.push_back(name); }
            } catch (...) {
                for (auto it = held.rbegin(); it != held.rend(); ++it) {
                    try { free_key(*it); } catch (...) {}
                }
                throw;
            }
            KeyRelease release{this, names};
            co_await sleep_for(node.has_duration ? node.duration : 0.06);
            break;
        }
        case StmtKind::Cursor: {
            if (node.a != "glide") {
                execute_pure(node, context);
                break;
            }
            Value x_value;
            Value y_value;
            LOOMWRIGHT_EVAL(node.exprs.at(0), context, x_value);
            LOOMWRIGHT_EVAL(node.exprs.at(1), context, y_value);
            const int x = static_cast<int>(x_value.as_int());
            const int y = static_cast<int>(y_value.as_int());
            co_await glide(x, y, node.duration);
            break;
        }
        case StmtKind::If: {
            Value condition_value;
            LOOMWRIGHT_EVAL(node.exprs.at(0), context, condition_value);
            co_await execute_nodes(condition_value.truthy() ? node.body : node.otherwise, context);
            break;
        }
        case StmtKind::Repeat: {
            Value count_value;
            LOOMWRIGHT_EVAL(node.exprs.at(0), context, count_value);
            const long long count = count_value.as_int();
            if (count < 0 || count > kMaxLoop) {
                throw RuntimeErrorLoomwright("repeat count must be from 0 to " + std::to_string(kMaxLoop));
            }
            ++loop_depth_;
            const auto depth = make_scope_exit([this] { --loop_depth_; });
            for (long long index = 0; index < count; ++index) {
                if (scheduler_.stop_requested()) break;
                try {
                    co_await execute_nodes(node.body, context);
                } catch (const BreakSignal&) {
                    break;
                } catch (const ContinueSignal&) {
                    continue;
                }
            }
            break;
        }
        case StmtKind::While: {
            long long iterations = 0;
            ++loop_depth_;
            const auto depth = make_scope_exit([this] { --loop_depth_; });
            Value condition;
            while (true) {
                LOOMWRIGHT_EVAL(node.exprs.at(0), context, condition);
                if (!condition.truthy()) break;
                if (scheduler_.stop_requested()) break;
                ++iterations;
                if (iterations > kMaxLoop) {
                    throw RuntimeErrorLoomwright("while loop exceeded safety limit (" + std::to_string(kMaxLoop) +
                                                 " iterations)");
                }
                try {
                    co_await execute_nodes(node.body, context);
                } catch (const BreakSignal&) {
                    break;
                } catch (const ContinueSignal&) {
                    continue;
                }
            }
            break;
        }
        case StmtKind::For: {
            ++loop_depth_;
            const auto depth = make_scope_exit([this] { --loop_depth_; });
            if (node.b == "in") {
                Value items;
                LOOMWRIGHT_EVAL(node.exprs.at(0), context, items);
                if (!items.is_list()) {
                    throw RuntimeErrorLoomwright(std::string("for ... in requires a list, got ") + type_name(items));
                }
                // Iterate a snapshot so the body may change the list.
                const std::vector<Value> snapshot = items.list_items();
                long long iterations = 0;
                for (const Value& item : snapshot) {
                    if (scheduler_.stop_requested()) break;
                    if (++iterations > kMaxLoop) {
                        throw RuntimeErrorLoomwright("for loop exceeded safety limit (" + std::to_string(kMaxLoop) +
                                                      " iterations)");
                    }
                    assign_variable(node.a, item);
                    try {
                        co_await execute_nodes(node.body, context);
                    } catch (const BreakSignal&) {
                        break;
                    } catch (const ContinueSignal&) {
                        continue;
                    }
                }
            } else {
                Value start_value;
                Value end_value;
                LOOMWRIGHT_EVAL(node.exprs.at(0), context, start_value);
                LOOMWRIGHT_EVAL(node.exprs.at(1), context, end_value);
                const long long start = start_value.as_int();
                const long long end = end_value.as_int();
                long long step = node.b == "downto" ? -1 : 1;
                if (node.exprs.size() > 2) {
                    Value step_value;
                    LOOMWRIGHT_EVAL(node.exprs.at(2), context, step_value);
                    step = step_value.as_int();
                }
                if (step == 0) throw RuntimeErrorLoomwright("for loop step must not be zero");
                long long iterations = 0;
                for (long long value = start; step > 0 ? value <= end : value >= end; value += step) {
                    if (scheduler_.stop_requested()) break;
                    if (++iterations > kMaxLoop) {
                        throw RuntimeErrorLoomwright("for loop exceeded safety limit (" + std::to_string(kMaxLoop) +
                                                      " iterations)");
                    }
                    assign_variable(node.a, Value::of_int(value));
                    try {
                        co_await execute_nodes(node.body, context);
                    } catch (const BreakSignal&) {
                        break;
                    } catch (const ContinueSignal&) {
                        continue;
                    }
                }
            }
            break;
        }
        default: {
            // The statement's expression may wait: evaluate it and run the
            // statement's action.
            Value first;
            Value second;
            if (!node.exprs.empty()) LOOMWRIGHT_EVAL(node.exprs.at(0), context, first);
            if (node.exprs.size() > 1) LOOMWRIGHT_EVAL(node.exprs.at(1), context, second);
            run_action(node, first, second, context);
            break;
        }
    }
}

bool Runtime::lookup_variable(const std::string& name, Value& out) const {
    // Most scripts run at the top level, so there is no function scope to
    // look in.
    if (!function_scopes_.empty()) [[unlikely]] {
        const auto local = function_scopes_.back().find(name);
        if (local != function_scopes_.back().end()) {
            out = local->second;
            return true;
        }
    }
    const auto found = variables_.find(name);
    if (found == variables_.end()) return false;
    out = found->second;
    return true;
}

void Runtime::assign_variable(const std::string& name, Value value) {
    if (!function_scopes_.empty()) {
        function_scopes_.back()[name] = std::move(value);
        return;
    }
    variables_[name] = std::move(value);
}

Task Runtime::call_user_function(const Function& function, std::vector<Value> args, const EventContext& context,
                                 Value& out) {
    if (args.size() != function.params.size()) {
        throw RuntimeErrorLoomwright("Function " + quote_string(function.name) + "() takes " +
                                  std::to_string(function.params.size()) + " argument(s), got " +
                                  std::to_string(args.size()));
    }
    if (function_depth_ >= kMaxCallDepth) {
        throw RuntimeErrorLoomwright("Maximum function call depth exceeded (" + std::to_string(kMaxCallDepth) + ")");
    }
    ++function_depth_;
    const auto depth = make_scope_exit([this] { --function_depth_; });

    // A function has no loop of its own, so `break` and `continue` inside it
    // are errors even when the call sits in a loop.
    const int caller_loop_depth = loop_depth_;
    loop_depth_ = 0;
    const auto restore_loop_depth = make_scope_exit([this, caller_loop_depth] { loop_depth_ = caller_loop_depth; });

    // The function gets its own scope: the parameters are seeded into it and
    // every assignment inside the body stays there.
    VariableMap scope;
    for (size_t index = 0; index < function.params.size(); ++index) {
        scope[function.params[index]] = std::move(args[index]);
    }
    function_scopes_.push_back(std::move(scope));
    const auto pop = make_scope_exit([this] { function_scopes_.pop_back(); });

    out = Value::none();
    try {
        co_await execute_nodes(function.body, context);
    } catch (const ReturnSignal& signal) {
        out = signal.value;
    }
}

Value Runtime::call_builtin(Builtin builtin, const std::string& name, std::vector<Value> args, const EventContext& context) {
    const auto require = [&](size_t count) {
        if (args.size() == count) return;
        throw RuntimeErrorLoomwright("Invalid arguments for " + name + "(): expected " + std::to_string(count) +
                                  " argument(s), got " + std::to_string(args.size()));
    };
    const auto require_string = [&](size_t index) -> const std::string& {
        if (args[index].type != Value::Type::Str) {
            throw RuntimeErrorLoomwright(name + "() expects a string, got " + type_name(args[index]));
        }
        return args[index].text;
    };
    const auto find_gui_widget = [&](const std::string& raw_id) -> GuiHost* {
        const std::string id = lower_ascii(trim_ascii(raw_id));
        for (auto& host : gui_hosts_) {
            if (host->has_widget(id)) return host.get();
        }
        return nullptr;
    };

    switch (builtin) {
    case Builtin::Min: case Builtin::Max: {
        if (args.empty()) {
            throw RuntimeErrorLoomwright("Invalid arguments for " + name + "(): expected at least 1 argument");
        }
        Value best = args.front();
        for (size_t index = 1; index < args.size(); ++index) {
            const bool replace = builtin == Builtin::Min ? compare_values(args[index], best) < 0
                                               : compare_values(best, args[index]) < 0;
            if (replace) best = args[index];
        }
        return best;
    }
    case Builtin::Abs: {
        require(1);
        if (args[0].type == Value::Type::Float) return Value::of_float(std::fabs(args[0].real));
        const long long value = args[0].as_int();
        return Value::of_int(value < 0 ? -value : value);
    }
    case Builtin::Round: {
        require(1);
        return Value::of_int(static_cast<long long>(std::nearbyint(args[0].as_double())));
    }
    case Builtin::Floor: {
        require(1);
        return Value::of_int(static_cast<long long>(std::floor(args[0].as_double())));
    }
    case Builtin::Ceil: {
        require(1);
        return Value::of_int(static_cast<long long>(std::ceil(args[0].as_double())));
    }
    case Builtin::Sqrt: {
        require(1);
        const double value = args[0].as_double();
        if (value < 0.0) throw RuntimeErrorLoomwright("math domain error");
        return Value::of_float(std::sqrt(value));
    }
    case Builtin::Clamp: {
        require(3);
        Value result = args[0];
        if (compare_values(result, args[2]) > 0) result = args[2];
        if (compare_values(result, args[1]) < 0) result = args[1];
        return result;
    }
    case Builtin::Int: {
        require(1);
        return Value::of_int(args[0].as_int());
    }
    case Builtin::Float: {
        require(1);
        return Value::of_float(args[0].as_double());
    }
    case Builtin::IsNumber: {
        require(1);
        try {
            const std::string value = trim_ascii(args[0].to_string());
            size_t used = 0;
            const double number = std::stod(value, &used);
            return Value::of_bool(used == value.size() && std::isfinite(number));
        } catch (const std::exception&) {
            return Value::of_bool(false);
        }
    }
    case Builtin::Str: {
        require(1);
        return Value::of_str(args[0].to_string());
    }
    case Builtin::Bool: {
        require(1);
        return Value::of_bool(args[0].truthy());
    }
    case Builtin::Len: {
        require(1);
        if (args[0].type == Value::Type::List) {
            return Value::of_int(static_cast<long long>(args[0].list_items().size()));
        }
        if (args[0].type != Value::Type::Str) {
            throw RuntimeErrorLoomwright(std::string("object of type '") + type_name(args[0]) +
                                      "' has no len()");
        }
        return Value::of_int(static_cast<long long>(utf8_length(args[0].text)));
    }
    case Builtin::Random: {
        require(0);
        return Value::of_float(std::generate_canonical<double, 53>(random_engine_));
    }
    case Builtin::Randint: {
        require(2);
        const long long low = args[0].as_int();
        const long long high = args[1].as_int();
        if (low > high) throw RuntimeErrorLoomwright("empty range for randrange()");
        std::uniform_int_distribution<long long> distribution(low, high);
        return Value::of_int(distribution(random_engine_));
    }
    case Builtin::Elapsed: {
        require(0);
        return Value::of_float(scheduler_.now() - started_at_);
    }
    case Builtin::KeyDown: {
        require(1);
        return Value::of_bool(pressed_keys_.find(canonical_key(args[0].to_string())) != pressed_keys_.end());
    }
    case Builtin::MouseHeld: {
        require(1);
        const auto found = button_refs_.find(lower_ascii(args[0].to_string()));
        return Value::of_bool(found != button_refs_.end() && found->second > 0);
    }
    case Builtin::SuspendHotkeys: {
        require(1);
        if (args[0].truthy()) {
            suspended_hotkeys_ = true;
            suspended_exempt_hotkeys_.clear();
            if (context.event_kind == "key" || context.event_kind == "mouse") {
                // The binding that suspended the script stays enabled, so it
                // can turn the suspension off again.
                const std::string id = context.hotkey_id.empty()
                                           ? (context.event_kind == "key"
                                                  ? canonical_key(context.key)
                                                  : canonical_mouse_event_key(context.key))
                                           : context.hotkey_id;
                if (!id.empty()) suspended_exempt_hotkeys_.insert(id);
            }
        } else {
            suspended_hotkeys_ = false;
            suspended_exempt_hotkeys_.clear();
        }
        return Value::of_bool(suspended_hotkeys_);
    }
    case Builtin::HotkeysSuspended: {
        require(0);
        return Value::of_bool(suspended_hotkeys_);
    }
    case Builtin::SetHotkeyEnabled: {
        require(2);
        const std::string id = hotkey_id_for_name(args[0].to_string());
        if (args[1].truthy()) {
            disabled_hotkeys_.erase(id);
        } else {
            disabled_hotkeys_.insert(id);
        }
        return Value::of_bool(true);
    }
    case Builtin::HotkeyEnabled: {
        require(1);
        const std::string id = hotkey_id_for_name(args[0].to_string());
        const bool disabled = disabled_hotkeys_.find(id) != disabled_hotkeys_.end();
        const bool suspended = suspended_hotkeys_ &&
                               suspended_exempt_hotkeys_.find(id) == suspended_exempt_hotkeys_.end();
        return Value::of_bool(!disabled && !suspended);
    }
    case Builtin::GuiValue: {
        require(1);
        const std::string id = lower_ascii(trim_ascii(args[0].to_string()));
        GuiHost* host = find_gui_widget(id);
        if (host == nullptr) {
            throw RuntimeErrorLoomwright("GUI widget " + quote_string(id) + " is not declared in a gui block.");
        }
        return Value::of_str(host->value(id));
    }
    case Builtin::GuiSetText: {
        require(2);
        const std::string id = lower_ascii(trim_ascii(args[0].to_string()));
        GuiHost* host = find_gui_widget(id);
        if (host == nullptr) {
            throw RuntimeErrorLoomwright("GUI widget " + quote_string(id) + " is not declared in a gui block.");
        }
        host->set_text(id, args[1].to_string());
        return Value::of_bool(true);
    }
    case Builtin::GuiSetVisible: {
        require(2);
        const std::string title = trim_ascii(args[0].to_string());
        bool found = false;
        for (auto& host : gui_hosts_) {
            if (host->title() == title) {
                host->set_visible(args[1].truthy());
                found = true;
            }
        }
        if (!found) {
            throw RuntimeErrorLoomwright("GUI panel " + quote_string(title) + " is not declared in a gui block.");
        }
        return Value::of_bool(true);
    }
    case Builtin::ActiveWindowTitle: {
        require(0);
        return Value::of_str(active_window_info().at("title"));
    }
    case Builtin::ActiveWindowClass: {
        require(0);
        return Value::of_str(active_window_info().at("class"));
    }
    case Builtin::ActiveWindowProcess: {
        require(0);
        return Value::of_str(active_window_info().at("process"));
    }
    case Builtin::ActiveWindowPid: {
        require(0);
        const std::string value = active_window_info().at("pid");
        if (value.empty()) return Value::of_int(0);
        try {
            return Value::of_int(std::stoll(value));
        } catch (const std::exception&) {
            return Value::of_int(0);
        }
    }
    case Builtin::GuiAlert: {
        if (args.size() < 1 || args.size() > 2) {
            throw RuntimeErrorLoomwright("Invalid arguments for " + name + "(): expected 1 or 2 argument(s), got " +
                                      std::to_string(args.size()));
        }
        gui_alert(args[0].to_string(), args.size() == 2 ? args[1].to_string() : script_.name);
        return Value::of_bool(true);
    }
    case Builtin::GuiInput: {
        if (args.size() < 1 || args.size() > 3) {
            throw RuntimeErrorLoomwright("Invalid arguments for " + name + "(): expected 1 to 3 argument(s), got " +
                                      std::to_string(args.size()));
        }
        return Value::of_str(gui_input(args[0].to_string(),
                                       args.size() > 1 ? args[1].to_string() : std::string(),
                                       args.size() > 2 ? args[2].to_string() : script_.name));
    }
    case Builtin::GuiConfirm: {
        if (args.size() < 1 || args.size() > 2) {
            throw RuntimeErrorLoomwright("Invalid arguments for " + name + "(): expected 1 or 2 argument(s), got " +
                                      std::to_string(args.size()));
        }
        return Value::of_bool(gui_confirm(args[0].to_string(),
                                           args.size() == 2 ? args[1].to_string() : script_.name));
    }
    case Builtin::ScreenWidth: {
        require(0);
        return Value::of_int(width_);
    }
    case Builtin::ScreenHeight: {
        require(0);
        return Value::of_int(height_);
    }

    // Strings.
    case Builtin::Upper: {
        require(1);
        std::string text = require_string(0);
        for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return Value::of_str(std::move(text));
    }
    case Builtin::Lower: {
        require(1);
        std::string text = require_string(0);
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return Value::of_str(std::move(text));
    }
    case Builtin::Trim: {
        require(1);
        return Value::of_str(trim_ascii(require_string(0)));
    }
    case Builtin::StartsWith: {
        require(2);
        const std::string& text = require_string(0);
        const std::string& prefix = require_string(1);
        return Value::of_bool(text.starts_with(prefix));
    }
    case Builtin::EndsWith: {
        require(2);
        const std::string& text = require_string(0);
        const std::string& suffix = require_string(1);
        if (suffix.size() > text.size()) return Value::of_bool(false);
        return Value::of_bool(text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0);
    }
    case Builtin::Contains: {
        require(2);
        if (args[0].is_list()) {
            for (const Value& item : args[0].list_items()) {
                if (value_equals(item, args[1])) return Value::of_bool(true);
            }
            return Value::of_bool(false);
        }
        const std::string& text = require_string(0);
        return Value::of_bool(text.find(require_string(1)) != std::string::npos);
    }
    case Builtin::Replace: {
        require(3);
        const std::string& text = require_string(0);
        const std::string& from = require_string(1);
        const std::string& to = require_string(2);
        if (from.empty()) return Value::of_str(text);
        std::string out;
        size_t start = 0;
        while (true) {
            const size_t found = text.find(from, start);
            if (found == std::string::npos) {
                out.append(text, start, std::string::npos);
                break;
            }
            out.append(text, start, found - start);
            out += to;
            start = found + from.size();
        }
        return Value::of_str(std::move(out));
    }
    case Builtin::Substr: {
        if (args.size() < 2 || args.size() > 3) {
            throw RuntimeErrorLoomwright("Invalid arguments for substr(): expected 2 or 3 argument(s), got " +
                                      std::to_string(args.size()));
        }
        const std::string& text = require_string(0);
        const long long total = static_cast<long long>(utf8_length(text));
        long long start = args[1].as_int();
        if (start < 0) start += total;
        if (start < 0) start = 0;
        if (start > total) start = total;
        long long length = args.size() > 2 ? args[2].as_int() : total - start;
        if (length < 0) length = 0;
        if (start + length > total) length = total - start;
        const size_t begin = utf8_offset(text, static_cast<size_t>(start));
        const size_t end = utf8_offset(text, static_cast<size_t>(start + length));
        return Value::of_str(text.substr(begin, end - begin));
    }
    case Builtin::IndexOf: {
        require(2);
        const std::string& text = require_string(0);
        const std::string& needle = require_string(1);
        const size_t position = text.find(needle);
        if (position == std::string::npos) return Value::of_int(-1);
        return Value::of_int(static_cast<long long>(utf8_length(text.substr(0, position))));
    }

    // Lists.
    case Builtin::Join: {
        require(2);
        const std::string& separator = require_string(1);
        std::string out;
        const std::vector<Value>& items = args[0].list_items();
        for (size_t index = 0; index < items.size(); ++index) {
            if (index > 0) out += separator;
            if (items[index].type != Value::Type::Str) {
                throw RuntimeErrorLoomwright(std::string("join() expects a list of strings, got ") + type_name(items[index]));
            }
            out += items[index].text;
        }
        return Value::of_str(std::move(out));
    }
    case Builtin::Split: {
        require(2);
        const std::string& text = require_string(0);
        const std::string& separator = require_string(1);
        if (separator.empty()) throw RuntimeErrorLoomwright("split() separator cannot be empty");
        std::vector<Value> parts;
        size_t start = 0;
        while (true) {
            const size_t found = text.find(separator, start);
            if (found == std::string::npos) {
                parts.push_back(Value::of_str(text.substr(start)));
                break;
            }
            parts.push_back(Value::of_str(text.substr(start, found - start)));
            start = found + separator.size();
        }
        return Value::of_list(std::move(parts));
    }
    case Builtin::Append: {
        require(2);
        std::vector<Value>& items = args[0].list_items();
        items.push_back(args[1]);
        return args[0];
    }
    case Builtin::Sort: {
        require(1);
        std::vector<Value>& items = args[0].list_items();
        std::stable_sort(items.begin(), items.end(),
                         [](const Value& left, const Value& right) { return compare_values(left, right) < 0; });
        return args[0];
    }

    default:
        throw RuntimeErrorLoomwright("Function " + quote_string(name) + " is declared but not implemented");
    }
}

Task Runtime::eval(const ExprPtr& expr, const EventContext& context, Value& out) {
    if (!expr->may_wait) [[likely]] {
        // Nothing in this expression can wait, so it is evaluated without a
        // coroutine: no frame is allocated and the script keeps running in
        // the same stack frame.
        out = eval_pure(*expr, context);
        co_return;
    }
    switch (expr->kind) {
        case Expr::Kind::Literal:
        case Expr::Kind::Variable:
            out = eval_pure(*expr, context);
            break;
        case Expr::Kind::List: {
            std::vector<Value> values;
            values.reserve(expr->args.size());
            for (const auto& argument : expr->args) {
                Value value;
                LOOMWRIGHT_EVAL(argument, context, value);
                values.push_back(std::move(value));
            }
            out = Value::of_list(std::move(values));
            break;
        }
        case Expr::Kind::Index: {
            Value base;
            Value index_value;
            LOOMWRIGHT_EVAL(expr->args.at(0), context, base);
            LOOMWRIGHT_EVAL(expr->args.at(1), context, index_value);
            if (!base.is_list()) {
                throw RuntimeErrorLoomwright(std::string("only a list can be indexed, got ") + type_name(base));
            }
            const std::vector<Value>& items = base.list_items();
            long long index = index_value.as_int();
            if (index < 0) index += static_cast<long long>(items.size());
            if (index < 0 || index >= static_cast<long long>(items.size())) [[unlikely]] {
                throw RuntimeErrorLoomwright("list index out of range");
            }
            out = items[static_cast<size_t>(index)];
            break;
        }
        case Expr::Kind::Unary: {
            Value value;
            LOOMWRIGHT_EVAL(expr->args.at(0), context, value);
            out = apply_unary(expr->unary_op, value);
            break;
        }
        case Expr::Kind::Binary: {
            const BinaryOp op = expr->binary_op;
            if (op == BinaryOp::And || op == BinaryOp::Or) {
                const bool is_and = op == BinaryOp::And;
                Value left;
                LOOMWRIGHT_EVAL(expr->args.at(0), context, left);
                if (is_and ? !left.truthy() : left.truthy()) {
                    out = Value::of_bool(!is_and);
                    break;
                }
                Value right;
                LOOMWRIGHT_EVAL(expr->args.at(1), context, right);
                out = Value::of_bool(right.truthy());
                break;
            }
            Value left;
            Value right;
            LOOMWRIGHT_EVAL(expr->args.at(0), context, left);
            LOOMWRIGHT_EVAL(expr->args.at(1), context, right);
            out = apply_binary(op, left, right);
            break;
        }
        case Expr::Kind::Call: {
            std::vector<Value> args;
            args.reserve(expr->args.size());
            for (const auto& argument : expr->args) {
                Value value;
                LOOMWRIGHT_EVAL(argument, context, value);
                args.push_back(std::move(value));
            }
            if (expr->builtin != Builtin::None) {
                out = call_builtin(expr->builtin, expr->name, std::move(args), context);
                break;
            }
            const auto user_function = functions_.find(expr->name);
            if (user_function == functions_.end()) {
                throw RuntimeErrorLoomwright("Unknown function " + quote_string(expr->name));
            }
            Value result;
            co_await call_user_function(*user_function->second, std::move(args), context, result);
            out = std::move(result);
            break;
        }
    }
}

Value Runtime::eval_pure(const Expr& expr, const EventContext& context) {
    switch (expr.kind) {
        case Expr::Kind::Literal:
            return expr.literal;
        case Expr::Kind::Variable: {
            if (expr.name == "event_key") return Value::of_str(context.key);
            if (expr.name == "event_edge") return Value::of_str(context.edge);
            Value value;
            if (!lookup_variable(expr.name, value)) [[unlikely]] {
                throw RuntimeErrorLoomwright("Unknown variable " + quote_string(expr.name));
            }
            return value;
        }
        case Expr::Kind::List: {
            std::vector<Value> values;
            values.reserve(expr.args.size());
            for (const auto& argument : expr.args) {
                values.push_back(eval_pure(*argument, context));
            }
            return Value::of_list(std::move(values));
        }
        case Expr::Kind::Index: {
            const Value base = eval_pure(*expr.args.at(0), context);
            const Value index_value = eval_pure(*expr.args.at(1), context);
            if (!base.is_list()) {
                throw RuntimeErrorLoomwright(std::string("only a list can be indexed, got ") + type_name(base));
            }
            const std::vector<Value>& items = base.list_items();
            long long index = index_value.as_int();
            if (index < 0) index += static_cast<long long>(items.size());
            if (index < 0 || index >= static_cast<long long>(items.size())) [[unlikely]] {
                throw RuntimeErrorLoomwright("list index out of range");
            }
            return items[static_cast<size_t>(index)];
        }
        case Expr::Kind::Unary:
            return apply_unary(expr.unary_op, eval_pure(*expr.args.at(0), context));
        case Expr::Kind::Binary: {
            const BinaryOp op = expr.binary_op;
            if (op == BinaryOp::And || op == BinaryOp::Or) {
                const bool is_and = op == BinaryOp::And;
                const Value left = eval_pure(*expr.args.at(0), context);
                if (is_and ? !left.truthy() : left.truthy()) return Value::of_bool(!is_and);
                return Value::of_bool(eval_pure(*expr.args.at(1), context).truthy());
            }
            return apply_binary(op, eval_pure(*expr.args.at(0), context), eval_pure(*expr.args.at(1), context));
        }
        case Expr::Kind::Call: {
            // A built-in function never waits, so it is evaluated here. A
            // call to a user-defined function may wait, and the parser marks
            // such an expression so it never reaches this path.
            if (expr.builtin == Builtin::None) {
                throw RuntimeErrorLoomwright("internal error: a function call cannot be evaluated without waiting");
            }
            std::vector<Value> args;
            args.reserve(expr.args.size());
            for (const auto& argument : expr.args) {
                args.push_back(eval_pure(*argument, context));
            }
            return call_builtin(expr.builtin, expr.name, std::move(args), context);
        }
    }
    return Value::none();
}

void Runtime::hold_button(const std::string& owner, const std::string& button) {
    const auto existing = held_tokens_.find(owner);
    if (existing != held_tokens_.end()) {
        if (existing->second != button) {
            throw RuntimeErrorLoomwright("Hold token " + quote_string(owner) + " already belongs to " +
                                      existing->second);
        }
        return;
    }
    const auto reference = button_refs_.find(button);
    const int count = reference != button_refs_.end() ? reference->second : 0;
    if (count == 0) backend_->button(button, true);
    held_tokens_[owner] = button;
    button_refs_[button] = count + 1;
}

void Runtime::free_button(const std::string& owner) {
    const auto held = held_tokens_.find(owner);
    if (held == held_tokens_.end()) return;
    const std::string button = held->second;

    const auto reference = button_refs_.find(button);
    const int count = (reference != button_refs_.end() ? reference->second : 1) - 1;
    if (count <= 0) {
        backend_->button(button, false);
        button_refs_.erase(button);
    } else {
        button_refs_[button] = count;
    }
    held_tokens_.erase(held);
}

void Runtime::hold_key(const std::string& name) {
    const auto found = key_refs_.find(name);
    const int count = found != key_refs_.end() ? found->second : 0;
    if (count == 0) backend_->key(name, true);
    key_refs_[name] = count + 1;
}

void Runtime::free_key(const std::string& name) {
    const auto found = key_refs_.find(name);
    const int count = (found != key_refs_.end() ? found->second : 0) - 1;
    if (count <= 0) {
        if (found != key_refs_.end()) {
            backend_->key(name, false);
            key_refs_.erase(found);
        }
    } else {
        key_refs_[name] = count;
    }
}

void Runtime::release_all_holds() {
    if (!backend_) return;
    std::set<std::string> buttons;
    for (const auto& entry : held_tokens_) buttons.insert(entry.second);
    for (const auto& button : buttons) {
        try {
            backend_->button(button, false);
        } catch (const std::exception&) {
        }
    }
    for (const auto& entry : key_refs_) {
        try {
            backend_->key(entry.first, false);
        } catch (const std::exception&) {
        }
    }
    held_tokens_.clear();
    button_refs_.clear();
    key_refs_.clear();
}

}  // namespace loomwright
