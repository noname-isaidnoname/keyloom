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
#include "subprocess.hpp"

namespace keyloom {
namespace {

struct SignalScope {
    sigset_t previous{};
    int fd = -1;

    SignalScope() {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        if (::sigprocmask(SIG_BLOCK, &mask, &previous) < 0)
            throw RuntimeErrorKeyloom("cannot block signals: " + std::string(std::strerror(errno)));
        fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (fd < 0) {
            const int error = errno;
            ::sigprocmask(SIG_SETMASK, &previous, nullptr);
            throw RuntimeErrorKeyloom("cannot create signal fd: " + std::string(std::strerror(error)));
        }
    }
    ~SignalScope() {
        if (fd >= 0) ::close(fd);
        ::sigprocmask(SIG_SETMASK, &previous, nullptr);
    }
    SignalScope(const SignalScope&) = delete;
    SignalScope& operator=(const SignalScope&) = delete;
};

const char* py_type_name(const Value& value) {
    switch (value.type) {
        case Value::Type::Int: return "int";
        case Value::Type::Float: return "float";
        case Value::Type::Bool: return "bool";
        case Value::Type::Str: return "str";
        case Value::Type::List: return "list";
        case Value::Type::None: return "NoneType";
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

std::string lower_ascii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trim_ascii(std::string text) {
    size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) ++first;
    size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) --last;
    return text.substr(first, last - first);
}

std::string join_sorted(const std::set<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += ", ";
        out += value;
    }
    return out;
}

std::vector<std::string> split_on(const std::string& text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t found = text.find(separator, start);
        if (found == std::string::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, found - start));
        start = found + 1;
    }
    return parts;
}

struct SendKey {
    std::string key;
    bool shift;
};

std::optional<SendKey> printable_key(char c) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte >= 'a' && byte <= 'z') return SendKey{std::string(1, static_cast<char>(byte - 'a' + 'A')), false};
    if (byte >= 'A' && byte <= 'Z') return SendKey{std::string(1, static_cast<char>(byte)), true};
    if (byte >= '0' && byte <= '9') return SendKey{std::string(1, static_cast<char>(byte)), false};
    static const std::unordered_map<char, SendKey> kSymbols = {
        {' ',  {"SPACE", false}}, {'-',  {"MINUS", false}}, {'_', {"MINUS", true}},
        {'=',  {"EQUAL", false}}, {'+',  {"EQUAL", true}},
        {'[',  {"LEFTBRACE", false}}, {'{', {"LEFTBRACE", true}},
        {']',  {"RIGHTBRACE", false}}, {'}', {"RIGHTBRACE", true}},
        {'\\', {"BACKSLASH", false}}, {'|', {"BACKSLASH", true}},
        {';',  {"SEMICOLON", false}}, {':', {"SEMICOLON", true}},
        {'\'', {"APOSTROPHE", false}}, {'"', {"APOSTROPHE", true}},
        {',',  {"COMMA", false}},   {'<', {"COMMA", true}},
        {'.',  {"DOT", false}},     {'>', {"DOT", true}},
        {'/',  {"SLASH", false}},   {'?', {"SLASH", true}},
        {'`',  {"GRAVE", false}},   {'~', {"GRAVE", true}},
        {'!',  {"1", true}},        {'@', {"2", true}},
        {'#',  {"3", true}},        {'$', {"4", true}},
        {'%',  {"5", true}},        {'^', {"6", true}},
        {'&',  {"7", true}},        {'*', {"8", true}},
        {'(',  {"9", true}},        {')', {"0", true}},
    };
    const auto found = kSymbols.find(static_cast<char>(byte));
    if (found != kSymbols.end()) return found->second;
    return std::nullopt;
}

std::optional<std::string> send_modifier_for_prefix(char c) {
    switch (c) {
        case '^': return std::string("LEFTCTRL");
        case '!': return std::string("LEFTALT");
        case '+': return std::string("LEFTSHIFT");
        case '#': return std::string("LEFTMETA");
        default: return std::nullopt;
    }
}

std::vector<std::string> split_send_words(const std::string& text) {
    std::vector<std::string> words;
    size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) ++start;
        if (start >= text.size()) break;
        size_t end = start;
        while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0) ++end;
        words.push_back(text.substr(start, end - start));
        start = end;
    }
    return words;
}

bool send_holds_key(const std::vector<std::string>& names, const std::string& key) {
    return std::find(names.begin(), names.end(), key) != names.end();
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
    throw RuntimeErrorKeyloom(std::string("'<' not supported between instances of '") + py_type_name(left) +
                              "' and '" + py_type_name(right) + "'");
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

/// True when `event` is the binding for the physical key or mouse button `key`.
bool event_binds_key(const Node& event, const std::string& key) {
    if (event.event_kind == "key") return canonical_key(event.event_key) == key;
    if (event.event_kind == "mouse") return canonical_mouse_event_key(event.event_key) == key;
    return false;
}

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
    if (lower.rfind("mouse:", 0) == 0) return canonical_mouse_event_key(name.substr(6));
    if (lower.rfind("mouse_", 0) == 0) return canonical_mouse_event_key(name);
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
    const char* env_width = std::getenv("KEYLOOM_SCREEN_WIDTH");
    const char* env_height = std::getenv("KEYLOOM_SCREEN_HEIGHT");
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
        throw RuntimeErrorKeyloom("option screen must be a string such as '1920x1080'");
    }
    const std::string text = lower_ascii(value->text);
    if (text == "auto") return fallback;
    const size_t separator = text.find('x');
    if (separator == std::string::npos) {
        throw RuntimeErrorKeyloom("option screen must look like '1920x1080' or 'auto'");
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
        throw RuntimeErrorKeyloom("option screen must look like '1920x1080' or 'auto'");
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

void Runtime::say(const std::string& message) { std::cout << "[keyloom] " << message << '\n' << std::flush; }

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
    return "No accessible keyboard devices were found. Keyloom reads /dev/input/event* through evdev. "
           "On most distributions, add your user to the 'input' group (then log out/in), or create a "
           "narrow udev rule for your keyboard. Do not run the whole automation as root. "
           "Use `keyloom --devices` to inspect permissions.";
}

std::string Runtime::mouse_permission_help() const {
    return "No accessible mouse devices were found. Keyloom reads /dev/input/event* through evdev and "
           "needs a device that reports both BTN_LEFT and BTN_RIGHT. On most distributions, add your "
           "user to the 'input' group (then log out/in), or create a narrow udev rule for your mouse. "
           "Do not run the whole automation as root. Use `keyloom --devices` to inspect permissions.";
}

std::vector<std::unique_ptr<InputDevice>> Runtime::open_input_devices(bool include_mice) {
    std::vector<std::string> paths;
    if (!options_.input_device.empty()) {
        paths.push_back(options_.input_device);
    } else {
        paths = list_input_devices();
    }

    std::vector<std::unique_ptr<InputDevice>> devices;
    std::set<std::string> seen;
    for (const auto& path : paths) {
        if (!seen.insert(path).second) continue;
        auto device = InputDevice::open(path);
        if (!device) continue;
        const std::string name = lower_ascii(device->name());
        // Never react to our own injected devices, or to the evdev proxies the
        // Python remappers create when both tools run at once.
        if (name.find("keyloom") != std::string::npos || name.find("ydotool") != std::string::npos ||
            name.find("virtual pointer") != std::string::npos ||
            name.find("remap_clicks") != std::string::npos) {
            continue;
        }
        const bool keyboard = device->is_keyboard();
        const bool mouse = include_mice && device->is_mouse();
        if (!keyboard && !mouse) continue;
        devices.push_back(std::move(device));
    }
    return devices;
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
            throw RuntimeErrorKeyloom("Keyloom's live runtime is Linux-only.");
        }
        if (!backend_) backend_ = select_backend();
        running_ = true;

        std::string panic_text = "PAUSE";
        if (const Value* panic = script_.option("panic")) panic_text = panic->to_string();
        panic_key_ = lower_ascii(panic_text) == "none" ? std::string() : canonical_key(panic_text);
        consumed_keys_ = collect_consumed_keys();

        for (const auto& panel : script_.panels) {
            auto host = GuiHost::create(panel);
            host->set_event_handler([this](const GuiHost::Event& event) { dispatch_gui_event(event); });
            gui_hosts_.push_back(std::move(host));
        }
        for (auto& host : gui_hosts_) spawn_background(gui_loop(host.get()));

        signals = std::make_shared<SignalScope>();
        spawn_background(signal_loop(signals->fd, signals));

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
            throw RuntimeErrorKeyloom(input_permission_help());
        }
        if (has_mouse_events && !has_device([](const auto& device) { return device->is_mouse(); })) {
            throw RuntimeErrorKeyloom(mouse_permission_help());
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
                            throw RuntimeErrorKeyloom(
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
        if (!runtime_error_.empty()) throw RuntimeErrorKeyloom(runtime_error_);
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

Task Runtime::read_device(InputDevice* device, std::shared_ptr<PassthroughDevice> passthrough) {
    const std::string path = device->path();
    struct Guard {
        Runtime* runtime;
        InputDevice* device;
        EventSink* passthrough;
        std::string path;

        ~Guard() {
            std::exception_ptr failure;
            const auto remember = [&failure](auto operation) {
                try { operation(); } catch (...) { if (!failure) failure = std::current_exception(); }
            };
            remember([&] {
                release_input_capture(passthrough, [this] { device->ungrab(); }, [this] { device->close(); });
            });
            if (!runtime->scheduler_.stop_requested()) {
                remember([&] { runtime->disconnect_device(path); });
            }
            if (failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (const std::exception& error) {
                    runtime->runtime_error_ = "input disconnect cleanup failed for " + path + ": " + error.what();
                    runtime->say(runtime->runtime_error_);
                } catch (...) {
                    runtime->runtime_error_ = "input disconnect cleanup failed for " + path + ": unknown error";
                    runtime->say(runtime->runtime_error_);
                }
                runtime->scheduler_.request_stop();
            }
            runtime->device_down_.erase(path);
            runtime->device_raw_down_.erase(path);
            runtime->desynchronized_devices_.erase(path);
        }
    } guard{this, device, passthrough.get(), path};

    std::vector<unsigned char> buffer(sizeof(input_event) * 64);
    while (true) {
        co_await readable(device->fd());
        const ssize_t bytes = ::read(device->fd(), buffer.data(), buffer.size());
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            say("Input device " + path + " disconnected: " + std::strerror(errno));
            co_return;
        }
        if (bytes == 0) {
            say("Input device " + path + " disconnected: end of file");
            co_return;
        }
        const auto* raw = reinterpret_cast<const input_event*>(buffer.data());
        const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
        std::vector<RawEvent> events;
        events.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            events.push_back(RawEvent{raw[index].type, raw[index].code, raw[index].value});
        }
        process_events(events, passthrough.get(), path, [device] { return device->pressed_key_codes(); });
    }
}

Task Runtime::gui_loop(GuiHost* host) {
    while (host != nullptr && !host->is_closed() && !scheduler_.stop_requested()) {
        try {
            host->process_events();
        } catch (const std::exception& error) {
            runtime_error_ = std::string("GUI event handling failed: ") + error.what();
            say(runtime_error_);
            scheduler_.request_stop();
            co_return;
        }
        if (host->is_closed() || scheduler_.stop_requested()) break;
        const int fd = host->fd();
        if (fd < 0) break;
        try {
            co_await readable(fd);
        } catch (const TaskCancelled&) {
            throw;
        } catch (const std::exception& error) {
            if (host->is_closed() || scheduler_.stop_requested()) break;
            runtime_error_ = std::string("GUI connection failed: ") + error.what();
            say(runtime_error_);
            scheduler_.request_stop();
            break;
        }
    }
}

void Runtime::dispatch_gui_event(const GuiHost::Event& event) {
    const std::string edge = event.kind == GuiHost::EventKind::Click ? "clicked" : "close";
    const std::string key = event.kind == GuiHost::EventKind::Click ? event.widget_id : "window";
    bool handled = false;
    for (const auto& candidate : script_.events) {
        if (candidate.event_kind == "gui" && candidate.event_key == key && candidate.edge == edge) {
            handled = true;
            spawn_background(safe_dispatch(candidate, key, edge));
        }
    }
    if (!handled && event.kind == GuiHost::EventKind::Close) scheduler_.request_stop();
}

void Runtime::process_events(const std::vector<RawEvent>& events, EventSink* sink, const std::string& path,
                             const std::function<std::vector<int>()>& snapshot) {
    for (const auto& event : events) {
        if (event.type == EV_SYN && event.code == SYN_DROPPED) {
            desynchronized_devices_.insert(path);
            continue;
        }
        if (desynchronized_devices_.count(path) != 0) {
            if (event.type != EV_SYN || event.code != SYN_REPORT) continue;
            try {
                if (!snapshot) throw RuntimeErrorKeyloom("no key state snapshot available");
                const auto codes = snapshot();
                const std::set<int> current(codes.begin(), codes.end());
                const auto previous = device_raw_down_[path];
                std::vector<RawEvent> changes;
                for (int code : previous) {
                    if (current.count(code) == 0) changes.push_back({EV_KEY, code, 0});
                }
                for (int code : current) {
                    if (previous.count(code) == 0) changes.push_back({EV_KEY, code, 1});
                }
                changes.push_back({EV_SYN, SYN_REPORT, 0});
                desynchronized_devices_.erase(path);
                process_events(changes, sink, path, snapshot);
                if (scheduler_.stop_requested()) return;
                // EVIOCGKEY flushes queued EV_KEY events in the kernel, but cannot
                // flush events already copied into this read batch. All remaining
                // events in the batch predate the snapshot and must not be replayed
                // or processed, including non-key events.
                break;
            } catch (const std::exception& error) {
                runtime_error_ = "input resynchronization failed for " + path + ": " + error.what();
                say(runtime_error_);
                scheduler_.request_stop();
                return;
            }
        }
        if (sink != nullptr && !is_consumed_key_event(event.type, event.code)) {
            try {
                sink->write_event(event.type, event.code, event.value);
            } catch (const std::exception& error) {
                runtime_error_ = "input passthrough failed for " + path + ": " + error.what();
                say(runtime_error_);
                scheduler_.request_stop();
                return;
            }
        }
        if (event.type != EV_KEY || event.value == 2) continue;
        if (event.value == 1) device_raw_down_[path].insert(event.code);
        else if (event.value == 0) device_raw_down_[path].erase(event.code);
        else continue;
        const auto name = normalize_key_event(event.code);
        if (!name) continue;
        const std::string key = canonical_key(*name);
        if (options_.verbose) {
            say("input " + key + (event.value == 1 ? " down" : " up") + " from " + path);
        }
        try {
            physical_key(path, key, event.value == 1);
        } catch (const std::exception& error) {
            runtime_error_ = "input handling failed for " + path + ": " + error.what();
            say(runtime_error_);
            scheduler_.request_stop();
            return;
        }
        if (scheduler_.stop_requested()) return;
    }
}

void Runtime::seed_device_keys(const std::string& path, const std::vector<int>& codes) {
    for (int code : codes) {
        device_raw_down_[path].insert(code);
        const auto name = normalize_key_event(code);
        if (!name) continue;
        const std::string key = canonical_key(*name);
        device_down_[path].insert(key);
        pressed_keys_.insert(key);
    }
}

void Runtime::disconnect_device(const std::string& path) {
    std::exception_ptr failure;
    const auto found = device_down_.find(path);
    if (found != device_down_.end()) {
        const std::vector<std::string> released(found->second.begin(), found->second.end());
        for (const auto& key : released) {
            try { physical_key(path, key, false); } catch (...) { if (!failure) failure = std::current_exception(); }
        }
        device_down_.erase(path);
    }
    device_raw_down_.erase(path);
    desynchronized_devices_.erase(path);
    if (failure) std::rethrow_exception(failure);
}

void Runtime::physical_key(const std::string& path, const std::string& key, bool down) {
    auto& per_device = device_down_[path];
    if (down) {
        const bool already = pressed_keys_.find(key) != pressed_keys_.end();
        per_device.insert(key);
        pressed_keys_.insert(key);
        if (already) return;
        if (!panic_key_.empty() && key == panic_key_) {
            say("Panic key " + key + " pressed.");
            scheduler_.request_stop();
            return;
        }
        for (const auto& remap : script_.remaps) {
            if (canonical_key(remap.source) != key) continue;
            if (active_remaps_.find(key) != active_remaps_.end()) return;
            std::vector<std::string> names;
            for (const auto& part : split_on(remap.target, '+')) names.push_back(canonical_key(part));
            std::vector<std::string> held;
            try {
                for (const auto& name : names) {
                    hold_key(name);
                    held.push_back(name);
                }
            } catch (...) {
                for (auto it = held.rbegin(); it != held.rend(); ++it) {
                    try { free_key(*it); } catch (...) {}
                }
                throw;
            }
            active_remaps_.emplace(key, std::move(names));
            return;
        }
        for (const auto& event : script_.events) {
            if (!event_binds_key(event, key)) continue;
            // A chord only fires while every modifier of the chord is held.
            if (!chord_modifiers_held(event)) continue;
            if (!hotkey_enabled(event)) continue;
            if (event.edge == "down") {
                spawn_background(safe_dispatch(event, key, "down"));
            } else if (event.edge == "up") {
                active_hotkey_ups_[key].push_back(&event);
            } else if (event.edge == "held") {
                held_tasks_[{key, event.line}] = spawn_held(event, key);
            }
        }
    } else {
        per_device.erase(key);
        bool still_down = false;
        for (const auto& entry : device_down_) {
            if (entry.second.find(key) != entry.second.end()) still_down = true;
        }
        if (still_down || pressed_keys_.find(key) == pressed_keys_.end()) return;
        pressed_keys_.erase(key);
        const auto remap_it = active_remaps_.find(key);
        if (remap_it != active_remaps_.end()) {
            const std::vector<std::string> names = std::move(remap_it->second);
            active_remaps_.erase(remap_it);
            std::exception_ptr failure;
            for (auto it = names.rbegin(); it != names.rend(); ++it) {
                try { free_key(*it); } catch (...) { if (!failure) failure = std::current_exception(); }
            }
            if (failure) std::rethrow_exception(failure);
            return;
        }
        for (auto it = held_tasks_.begin(); it != held_tasks_.end();) {
            if (it->first.first == key) {
                scheduler_.cancel(it->second);
                it = held_tasks_.erase(it);
            } else {
                ++it;
            }
        }
        const auto active_up = active_hotkey_ups_.find(key);
        if (active_up != active_hotkey_ups_.end()) {
            const auto events = std::move(active_up->second);
            active_hotkey_ups_.erase(active_up);
            for (const Node* event : events) spawn_background(safe_dispatch(*event, key, "up"));
        }
    }
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
        if (scheduler_.stop_requested()) co_return;
        co_await execute(node, context);
    }
}

Task Runtime::execute(const Node& node, const EventContext& context) {
    switch (node.stmt) {
        case StmtKind::Cursor: {
            if (node.a == "warp") {
                Value x_value;
                Value y_value;
                co_await eval(node.exprs.at(0), context, x_value);
                co_await eval(node.exprs.at(1), context, y_value);
                const int x = static_cast<int>(x_value.as_int());
                const int y = static_cast<int>(y_value.as_int());
                backend_->warp(x, y);
                variables_["cursor_x"] = Value::of_int(x);
                variables_["cursor_y"] = Value::of_int(y);
            } else if (node.a == "move") {
                Value dx_value;
                Value dy_value;
                co_await eval(node.exprs.at(0), context, dx_value);
                co_await eval(node.exprs.at(1), context, dy_value);
                const int dx = static_cast<int>(dx_value.as_int());
                const int dy = static_cast<int>(dy_value.as_int());
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
            } else if (node.a == "glide") {
                Value x_value;
                Value y_value;
                co_await eval(node.exprs.at(0), context, x_value);
                co_await eval(node.exprs.at(1), context, y_value);
                const int x = static_cast<int>(x_value.as_int());
                const int y = static_cast<int>(y_value.as_int());
                co_await glide(x, y, node.duration);
            }
            break;
        }
        case StmtKind::Mouse: {
            const std::string& button = node.a;
            std::string owner = node.c;
            if (owner.empty()) {
                owner = "default:" + (context.key.empty() ? std::string("script") : context.key) + ":" + button;
            }
            if (node.b == "hold") {
                hold_button(owner, button);
            } else if (node.b == "free") {
                free_button(owner);
            } else {
                ++tap_counter_;
                const std::string tap_owner = "__keyloom_tap_" + std::to_string(tap_counter_);
                hold_button(tap_owner, button);
                ButtonRelease release{this, tap_owner};
                co_await sleep_for(node.has_duration ? node.duration : 0.06);
            }
            break;
        }
        case StmtKind::Key: {
            std::vector<std::string> names;
            for (const auto& part : split_on(node.a, '+')) names.push_back(canonical_key(part));
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
            } else {
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
            }
            break;
        }
        case StmtKind::Consume:
            // Consumption is collected before devices are grabbed so the first
            // key-down is swallowed; the declaration has no per-event effect.
            break;
        case StmtKind::Type: {
            Value text;
            co_await eval(node.exprs.at(0), context, text);
            backend_->type_text(text.to_string());
            break;
        }
        case StmtKind::Wait:
            {
                Value seconds_value;
                if (node.exprs.empty()) {
                    seconds_value = Value::of_float(node.duration);
                } else {
                    co_await eval(node.exprs.at(0), context, seconds_value);
                }
                const double seconds = seconds_value.as_double();
                if (!std::isfinite(seconds) || seconds < 0) throw RuntimeErrorKeyloom("wait duration must be finite and non-negative");
                co_await sleep_for(seconds);
            }
            break;
        case StmtKind::Scroll: {
            Value x_value;
            Value y_value;
            co_await eval(node.exprs.at(0), context, x_value);
            co_await eval(node.exprs.at(1), context, y_value);
            const int x = static_cast<int>(x_value.as_int());
            const int y = static_cast<int>(y_value.as_int());
            backend_->scroll(x, y);
            break;
        }
        case StmtKind::Log: {
            Value message;
            co_await eval(node.exprs.at(0), context, message);
            std::cout << message.to_string() << '\n' << std::flush;
            break;
        }
        case StmtKind::Notify: {
            Value message;
            co_await eval(node.exprs.at(0), context, message);
            notify_desktop(message.to_string());
            break;
        }
        case StmtKind::ClipWrite: {
            Value text;
            co_await eval(node.exprs.at(0), context, text);
            clipboard("write", text.to_string());
            break;
        }
        case StmtKind::ClipRead:
            assign_variable(node.a, Value::of_str(clipboard("read")));
            break;
        case StmtKind::Launch: {
            Value command;
            co_await eval(node.exprs.at(0), context, command);
            launch_program(command.to_string());
            break;
        }
        case StmtKind::Set: {
            Value value;
            co_await eval(node.exprs.at(0), context, value);
            assign_variable(node.a, std::move(value));
            break;
        }
        case StmtKind::If: {
            Value condition_value;
            co_await eval(node.exprs.at(0), context, condition_value);
            const bool condition = condition_value.truthy();
            co_await execute_nodes(condition ? node.body : node.otherwise, context);
            break;
        }
        case StmtKind::Repeat: {
            Value count_value;
            co_await eval(node.exprs.at(0), context, count_value);
            const long long count = count_value.as_int();
            if (count < 0 || count > kMaxLoop) {
                throw RuntimeErrorKeyloom("repeat count must be from 0 to " + std::to_string(kMaxLoop));
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
                co_await eval(node.exprs.at(0), context, condition);
                if (!condition.truthy()) break;
                if (scheduler_.stop_requested()) break;
                ++iterations;
                if (iterations > kMaxLoop) {
                    throw RuntimeErrorKeyloom("while loop exceeded safety limit (" + std::to_string(kMaxLoop) +
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
                co_await eval(node.exprs.at(0), context, items);
                if (!items.is_list()) {
                    throw RuntimeErrorKeyloom(std::string("for ... in requires a list, got ") + py_type_name(items));
                }
                // Iterate a snapshot so the body may change the list.
                const std::vector<Value> snapshot = items.list_items();
                long long iterations = 0;
                for (const Value& item : snapshot) {
                    if (scheduler_.stop_requested()) break;
                    if (++iterations > kMaxLoop) {
                        throw RuntimeErrorKeyloom("for loop exceeded safety limit (" + std::to_string(kMaxLoop) +
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
                co_await eval(node.exprs.at(0), context, start_value);
                co_await eval(node.exprs.at(1), context, end_value);
                const long long start = start_value.as_int();
                const long long end = end_value.as_int();
                long long step = node.b == "downto" ? -1 : 1;
                if (node.exprs.size() > 2) {
                    Value step_value;
                    co_await eval(node.exprs.at(2), context, step_value);
                    step = step_value.as_int();
                }
                if (step == 0) throw RuntimeErrorKeyloom("for loop step must not be zero");
                long long iterations = 0;
                for (long long value = start; step > 0 ? value <= end : value >= end; value += step) {
                    if (scheduler_.stop_requested()) break;
                    if (++iterations > kMaxLoop) {
                        throw RuntimeErrorKeyloom("for loop exceeded safety limit (" + std::to_string(kMaxLoop) +
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
        case StmtKind::Break:
            if (loop_depth_ == 0) throw RuntimeErrorKeyloom("break outside a loop");
            throw BreakSignal{};
        case StmtKind::Continue:
            if (loop_depth_ == 0) throw RuntimeErrorKeyloom("continue outside a loop");
            throw ContinueSignal{};
        case StmtKind::Return: {
            if (function_depth_ == 0) throw RuntimeErrorKeyloom("return outside a function");
            ReturnSignal signal;
            if (!node.exprs.empty()) {
                Value value;
                co_await eval(node.exprs.at(0), context, value);
                signal.value = std::move(value);
            }
            throw signal;
        }
        case StmtKind::Assert: {
            Value condition;
            co_await eval(node.exprs.at(0), context, condition);
            if (!condition.truthy()) {
                throw RuntimeErrorKeyloom("assertion failed at line " + std::to_string(node.line));
            }
            break;
        }
        case StmtKind::Send: {
            Value text;
            co_await eval(node.exprs.at(0), context, text);
            send_string(text.to_string());
            break;
        }
        case StmtKind::Call: {
            Value result;
            co_await eval(node.exprs.at(0), context, result);
            break;
        }
    }
}

void Runtime::send_target(const std::vector<std::string>& names, const std::vector<std::string>& active_modifiers,
                           const std::vector<std::string>& pending_modifiers, bool add_shift) {
    std::vector<std::string> chord = active_modifiers;
    chord.insert(chord.end(), pending_modifiers.begin(), pending_modifiers.end());
    if (add_shift) {
        const bool has_shift = send_holds_key(chord, "LEFTSHIFT") || send_holds_key(chord, "RIGHTSHIFT");
        if (!has_shift) chord.push_back("LEFTSHIFT");
    }
    chord.insert(chord.end(), names.begin(), names.end());
    std::vector<std::string> held;
    try {
        for (const auto& name : chord) {
            hold_key(name);
            held.push_back(name);
        }
    } catch (...) {
        for (auto it = held.rbegin(); it != held.rend(); ++it) {
            try { free_key(*it); } catch (...) {}
        }
        throw;
    }
    std::exception_ptr failure;
    for (auto it = chord.rbegin(); it != chord.rend(); ++it) {
        try { free_key(*it); } catch (...) { if (!failure) failure = std::current_exception(); }
    }
    if (failure) std::rethrow_exception(failure);
}

void Runtime::send_char(char c, const std::vector<std::string>& active_modifiers,
                       const std::vector<std::string>& pending_modifiers) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte >= 0x80) {
        throw RuntimeErrorKeyloom("send with held modifiers only supports ASCII; release the modifiers before typing non-ASCII text");
    }
    const auto key = printable_key(c);
    if (!key) {
        throw RuntimeErrorKeyloom("send cannot handle character " + py_repr_string(std::string(1, c)) +
                                  " while modifiers are held");
    }
    send_target({key->key}, active_modifiers, pending_modifiers, key->shift);
}

void Runtime::send_string(const std::string& text) {
    std::vector<std::string> active_modifiers;
    std::vector<std::string> pending_modifiers;
    std::string plain;
    const auto flush_plain = [&]() {
        if (plain.empty()) return;
        backend_->type_text(plain);
        plain.clear();
    };
    size_t index = 0;
    while (index < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        if (const auto modifier = send_modifier_for_prefix(static_cast<char>(byte))) {
            flush_plain();
            pending_modifiers.push_back(*modifier);
            ++index;
            continue;
        }
        if (byte == '{') {
            flush_plain();
            const size_t close = text.find('}', index + 1);
            if (close == std::string::npos) {
                throw RuntimeErrorKeyloom("send has an unclosed '{' token");
            }
            const std::string content = trim_ascii(text.substr(index + 1, close - index - 1));
            const std::vector<std::string> words = split_send_words(content);
            if (words.empty()) throw RuntimeErrorKeyloom("send has an empty {} token");
            const std::string& first = words.front();

            if (words.size() == 2) {
                const std::string action = lower_ascii(words.at(1));
                if (action == "down" || action == "up") {
                    if (!pending_modifiers.empty()) {
                        throw RuntimeErrorKeyloom("send hold/release tokens cannot be combined with pending ^, !, +, or # modifiers");
                    }
                    if (first.find('+') != std::string::npos) {
                        throw RuntimeErrorKeyloom("send hold/release tokens accept one key, not a chord");
                    }
                    const std::string name = canonical_key(first);
                    if (action == "down") {
                        hold_key(name);
                        active_modifiers.push_back(name);
                    } else {
                        const auto found = std::find(active_modifiers.rbegin(), active_modifiers.rend(), name);
                        if (found == active_modifiers.rend()) {
                            throw RuntimeErrorKeyloom("send releases " + name + ", but it is not held");
                        }
                        free_key(name);
                        active_modifiers.erase(std::next(found).base());
                    }
                    index = close + 1;
                    continue;
                }
            }

            int repeat = 1;
            if (words.size() == 2) {
                const std::string count_text = words.at(1);
                if (count_text.empty() ||
                    !std::all_of(count_text.begin(), count_text.end(),
                                 [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
                    throw RuntimeErrorKeyloom("send token {" + content + "} has a non-numeric repeat count");
                }
                repeat = std::stoi(count_text);
                if (repeat < 1 || repeat > 100) {
                    throw RuntimeErrorKeyloom("send repeat count must be between 1 and 100");
                }
            } else if (words.size() > 2) {
                throw RuntimeErrorKeyloom("send token {" + content + "} has too many words");
            }

            std::vector<std::string> names;
            if (first == "^" || first == "!" || first == "+" || first == "#" || first == "{" || first == "}") {
                names.clear();
                for (int count = 0; count < repeat; ++count) {
                    send_char(first.front(), active_modifiers, pending_modifiers);
                    pending_modifiers.clear();
                }
                index = close + 1;
                continue;
            }
            for (const auto& part : split_on(first, '+')) names.push_back(canonical_key(part));
            for (int count = 0; count < repeat; ++count) {
                send_target(names, active_modifiers, pending_modifiers, false);
            }
            pending_modifiers.clear();
            index = close + 1;
            continue;
        }
        if (byte < 32 || byte == 127) {
            throw RuntimeErrorKeyloom("send cannot send control byte " + std::to_string(byte) +
                                      "; use tokens such as {ENTER} and {TAB}");
        }
        if (pending_modifiers.empty() && active_modifiers.empty()) {
            plain.push_back(static_cast<char>(byte));
        } else {
            send_char(static_cast<char>(byte), active_modifiers, pending_modifiers);
            pending_modifiers.clear();
        }
        ++index;
    }
    flush_plain();
    if (!pending_modifiers.empty()) {
        throw RuntimeErrorKeyloom("send ends with unmatched ^, !, +, or # modifiers");
    }
}

Task Runtime::glide(int x, int y, double duration) {
    duration = std::max(0.0, std::min(duration, 30.0));
    // Start from Keyloom's last known cursor position, or the screen centre.
    const auto current_x = variables_.find("cursor_x");
    const auto current_y = variables_.find("cursor_y");
    const int start_x = current_x != variables_.end() ? static_cast<int>(current_x->second.as_int()) : width_ / 2;
    const int start_y = current_y != variables_.end() ? static_cast<int>(current_y->second.as_int()) : height_ / 2;
    const int steps = std::max(1, std::min(120, static_cast<int>(duration * 60)));
    const double step_delay = duration > 0.0 ? duration / steps : 0.0;

    for (int index = 1; index <= steps; ++index) {
        const double progress = static_cast<double>(index) / steps;
        const double eased = progress * progress * (3.0 - 2.0 * progress);
        const int px = static_cast<int>(std::nearbyint(start_x + (x - start_x) * eased));
        const int py = static_cast<int>(std::nearbyint(start_y + (y - start_y) * eased));
        backend_->warp(px, py);
        variables_["cursor_x"] = Value::of_int(px);
        variables_["cursor_y"] = Value::of_int(py);
        co_await sleep_for(step_delay);
    }
}

void Runtime::notify_desktop(const std::string& message) {
    const std::string executable = which("notify-send");
    if (executable.empty()) {
        say("notification: " + message);
        return;
    }
    std::string error;
    spawn_detached({executable, "Keyloom", message}, error);
}

void Runtime::launch_program(const std::string& command) {
    const Value* allowed = script_.option("allow_launch");
    if (allowed == nullptr || !allowed->truthy()) {
        throw RuntimeErrorKeyloom(
            "launch is disabled. Set `option allow_launch = true` to permit application launching.");
    }
    const std::vector<std::string> argv = split_shell_words(command);
    if (argv.empty()) throw RuntimeErrorKeyloom("launch command cannot be empty");
    std::string error;
    if (!spawn_detached(argv, error)) {
        throw RuntimeErrorKeyloom("could not launch " + py_repr_string(argv.front()) + ": " + error);
    }
}

std::string Runtime::clipboard(const std::string& action, const std::string& value) {
    const bool wayland = std::getenv("WAYLAND_DISPLAY") != nullptr;
    if (action == "write") {
        std::vector<std::string> command;
        if (wayland && !which("wl-copy").empty()) {
            command = {which("wl-copy")};
        } else if (!which("xclip").empty()) {
            command = {which("xclip"), "-selection", "clipboard"};
        } else if (!which("xsel").empty()) {
            command = {which("xsel"), "--clipboard", "--input"};
        }
        if (command.empty()) {
            throw RuntimeErrorKeyloom("Clipboard write needs wl-copy (Wayland), xclip, or xsel.");
        }
        const CommandResult result = run_command(command, value, 3000);
        if (result.returncode != 0) {
            std::string detail = result.err.empty() ? result.out : result.err;
            while (!detail.empty() && (detail.back() == '\n' || detail.back() == ' ')) detail.pop_back();
            throw RuntimeErrorKeyloom("Clipboard write failed: " +
                                      (detail.empty() ? "exit " + std::to_string(result.returncode) : detail));
        }
        return std::string();
    }

    std::vector<std::string> command;
    if (wayland && !which("wl-paste").empty()) {
        command = {which("wl-paste"), "--no-newline"};
    } else if (!which("xclip").empty()) {
        command = {which("xclip"), "-selection", "clipboard", "-o"};
    } else if (!which("xsel").empty()) {
        command = {which("xsel"), "--clipboard", "--output"};
    } else {
        throw RuntimeErrorKeyloom("Clipboard read needs wl-paste (Wayland), xclip, or xsel.");
    }
    const CommandResult result = run_command(command, std::string(), 3000);
    if (result.returncode != 0) {
        std::string detail = result.err.empty() ? result.out : result.err;
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == ' ')) detail.pop_back();
        throw RuntimeErrorKeyloom("Clipboard read failed: " +
                                  (detail.empty() ? "exit " + std::to_string(result.returncode) : detail));
    }
    return result.out;
}

void Runtime::gui_alert(const std::string& message, const std::string& title) {
    const std::string executable = which("zenity");
    if (!executable.empty()) {
        const CommandResult result =
            run_command({executable, "--info", "--title=" + title, "--text=" + message}, std::string(), 120000);
        if (result.returncode != 0 && !result.spawn_failed) {
            throw RuntimeErrorKeyloom("GUI alert failed with exit code " + std::to_string(result.returncode));
        }
        return;
    }
    const std::string kdialog = which("kdialog");
    if (!kdialog.empty()) {
        const CommandResult result =
            run_command({kdialog, "--title", title, "--msgbox", message}, std::string(), 120000);
        if (result.returncode != 0 && !result.spawn_failed) {
            throw RuntimeErrorKeyloom("GUI alert failed with exit code " + std::to_string(result.returncode));
        }
        return;
    }
    const std::string yad = which("yad");
    if (!yad.empty()) {
        const CommandResult result =
            run_command({yad, "--info", "--title=" + title, "--text=" + message}, std::string(), 120000);
        if (result.returncode != 0 && !result.spawn_failed) {
            throw RuntimeErrorKeyloom("GUI alert failed with exit code " + std::to_string(result.returncode));
        }
        return;
    }
    throw RuntimeErrorKeyloom("GUI dialogs need zenity, kdialog, or yad.");
}

std::string Runtime::gui_input(const std::string& prompt, const std::string& default_value, const std::string& title) {
    const std::string executable = which("zenity");
    if (!executable.empty()) {
        const CommandResult result = run_command(
            {executable, "--entry", "--title=" + title, "--text=" + prompt, "--entry-text=" + default_value},
            std::string(), 120000);
        if (result.returncode != 0) return std::string();
        std::string value = result.out;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        return value;
    }
    const std::string kdialog = which("kdialog");
    if (!kdialog.empty()) {
        const CommandResult result =
            run_command({kdialog, "--title", title, "--inputbox", prompt, default_value}, std::string(), 120000);
        if (result.returncode != 0) return std::string();
        std::string value = result.out;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        return value;
    }
    const std::string yad = which("yad");
    if (!yad.empty()) {
        const CommandResult result = run_command(
            {yad, "--entry", "--title=" + title, "--text=" + prompt, "--entry-text=" + default_value},
            std::string(), 120000);
        if (result.returncode != 0) return std::string();
        std::string value = result.out;
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
        return value;
    }
    throw RuntimeErrorKeyloom("GUI input needs zenity, kdialog, or yad.");
}

bool Runtime::gui_confirm(const std::string& question, const std::string& title) {
    const std::string executable = which("zenity");
    if (!executable.empty()) {
        const CommandResult result =
            run_command({executable, "--question", "--title=" + title, "--text=" + question}, std::string(), 120000);
        return result.returncode == 0;
    }
    const std::string kdialog = which("kdialog");
    if (!kdialog.empty()) {
        const CommandResult result =
            run_command({kdialog, "--title", title, "--yesno", question}, std::string(), 120000);
        return result.returncode == 0;
    }
    const std::string yad = which("yad");
    if (!yad.empty()) {
        const CommandResult result =
            run_command({yad, "--question", "--title=" + title, "--text=" + question}, std::string(), 120000);
        return result.returncode == 0;
    }
    throw RuntimeErrorKeyloom("GUI confirmation needs zenity, kdialog, or yad.");
}

std::map<std::string, std::string> Runtime::active_window_info() const {
    std::map<std::string, std::string> info;
    info["title"] = std::string();
    info["class"] = std::string();
    info["pid"] = std::string();
    info["process"] = std::string();

    const std::string xdotool = which("xdotool");
    if (!xdotool.empty()) {
        const CommandResult window_id = run_command({xdotool, "getactivewindow"}, std::string(), 250);
        if (window_id.returncode == 0) {
            std::string id = window_id.out;
            while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ' || id.back() == '\t')) {
                id.pop_back();
            }
            if (!id.empty()) {
                const CommandResult title = run_command({xdotool, "getwindowname", id}, std::string(), 250);
                if (title.returncode == 0) {
                    std::string value = title.out;
                    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
                    info["title"] = value;
                }
                const CommandResult window_class = run_command({xdotool, "getwindowclassname", id}, std::string(), 250);
                if (window_class.returncode == 0) {
                    std::string value = window_class.out;
                    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
                    info["class"] = value;
                }
                const CommandResult pid = run_command({xdotool, "getwindowpid", id}, std::string(), 250);
                if (pid.returncode == 0) {
                    std::string value = pid.out;
                    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
                                             value.back() == '\t')) {
                        value.pop_back();
                    }
                    if (!value.empty() &&
                        std::all_of(value.begin(), value.end(), [](char c) {
                            return std::isdigit(static_cast<unsigned char>(c)) != 0;
                        })) {
                        info["pid"] = value;
                    }
                }
            }
        }
    }

    if (info["pid"].empty()) {
        const std::string kdotool = which("kdotool");
        if (!kdotool.empty()) {
            const CommandResult active = run_command({kdotool, "getactivewindow"}, std::string(), 250);
            if (active.returncode == 0) {
                std::string id = active.out;
                while (!id.empty() &&
                       (id.back() == '\n' || id.back() == '\r' || id.back() == ' ' || id.back() == '\t')) {
                    id.pop_back();
                }
                if (!id.empty()) {
                    const CommandResult title = run_command({kdotool, "getwindowname", id}, std::string(), 250);
                    if (title.returncode == 0) {
                        std::string value = title.out;
                        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
                        info["title"] = value;
                    }
                    const CommandResult window_class = run_command({kdotool, "getwindowclassname", id}, std::string(), 250);
                    if (window_class.returncode == 0) {
                        std::string value = window_class.out;
                        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
                        info["class"] = value;
                    }
                    const CommandResult pid = run_command({kdotool, "getwindowpid", id}, std::string(), 250);
                    if (pid.returncode == 0) {
                        std::string value = pid.out;
                        while (!value.empty() &&
                               (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
                                value.back() == '\t')) {
                            value.pop_back();
                        }
                        if (!value.empty() &&
                            std::all_of(value.begin(), value.end(), [](char c) {
                                return std::isdigit(static_cast<unsigned char>(c)) != 0;
                            })) {
                            info["pid"] = value;
                        }
                    }
                }
            }
        }
    }

    if (!info["pid"].empty()) {
        std::ifstream comm("/proc/" + info["pid"] + "/comm");
        std::string process;
        comm >> process;
        if (!comm.fail()) info["process"] = process;
    }
    return info;
}

bool Runtime::lookup_variable(const std::string& name, Value& out) const {
    if (!function_scopes_.empty()) {
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
        throw RuntimeErrorKeyloom("Function " + py_repr_string(function.name) + "() takes " +
                                  std::to_string(function.params.size()) + " argument(s), got " +
                                  std::to_string(args.size()));
    }
    if (function_depth_ >= kMaxCallDepth) {
        throw RuntimeErrorKeyloom("Maximum function call depth exceeded (" + std::to_string(kMaxCallDepth) + ")");
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
    std::map<std::string, Value> scope;
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

Value Runtime::call_function(const std::string& name, std::vector<Value> args, const EventContext& context) {
    const auto require = [&](size_t count) {
        if (args.size() == count) return;
        throw RuntimeErrorKeyloom("Invalid arguments for " + name + "(): expected " + std::to_string(count) +
                                  " argument(s), got " + std::to_string(args.size()));
    };

    if (name == "min" || name == "max") {
        if (args.empty()) {
            throw RuntimeErrorKeyloom("Invalid arguments for " + name + "(): expected at least 1 argument");
        }
        Value best = args.front();
        for (size_t index = 1; index < args.size(); ++index) {
            const bool replace = name == "min" ? compare_values(args[index], best) < 0
                                               : compare_values(best, args[index]) < 0;
            if (replace) best = args[index];
        }
        return best;
    }
    if (name == "abs") {
        require(1);
        if (args[0].type == Value::Type::Float) return Value::of_float(std::fabs(args[0].real));
        const long long value = args[0].as_int();
        return Value::of_int(value < 0 ? -value : value);
    }
    if (name == "round") {
        require(1);
        return Value::of_int(static_cast<long long>(std::nearbyint(args[0].as_double())));
    }
    if (name == "floor") {
        require(1);
        return Value::of_int(static_cast<long long>(std::floor(args[0].as_double())));
    }
    if (name == "ceil") {
        require(1);
        return Value::of_int(static_cast<long long>(std::ceil(args[0].as_double())));
    }
    if (name == "sqrt") {
        require(1);
        const double value = args[0].as_double();
        if (value < 0.0) throw RuntimeErrorKeyloom("math domain error");
        return Value::of_float(std::sqrt(value));
    }
    if (name == "clamp") {
        require(3);
        Value result = args[0];
        if (compare_values(result, args[2]) > 0) result = args[2];
        if (compare_values(result, args[1]) < 0) result = args[1];
        return result;
    }
    if (name == "int") {
        require(1);
        return Value::of_int(args[0].as_int());
    }
    if (name == "float") {
        require(1);
        return Value::of_float(args[0].as_double());
    }
    if (name == "is_number") {
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
    if (name == "str") {
        require(1);
        return Value::of_str(args[0].to_string());
    }
    if (name == "bool") {
        require(1);
        return Value::of_bool(args[0].truthy());
    }
    if (name == "len") {
        require(1);
        if (args[0].type == Value::Type::List) {
            return Value::of_int(static_cast<long long>(args[0].list_items().size()));
        }
        if (args[0].type != Value::Type::Str) {
            throw RuntimeErrorKeyloom(std::string("object of type '") + py_type_name(args[0]) +
                                      "' has no len()");
        }
        return Value::of_int(static_cast<long long>(utf8_length(args[0].text)));
    }
    if (name == "random") {
        require(0);
        return Value::of_float(std::generate_canonical<double, 53>(random_engine_));
    }
    if (name == "randint") {
        require(2);
        const long long low = args[0].as_int();
        const long long high = args[1].as_int();
        if (low > high) throw RuntimeErrorKeyloom("empty range for randrange()");
        std::uniform_int_distribution<long long> distribution(low, high);
        return Value::of_int(distribution(random_engine_));
    }
    if (name == "elapsed") {
        require(0);
        return Value::of_float(scheduler_.now() - started_at_);
    }
    if (name == "key_down") {
        require(1);
        return Value::of_bool(pressed_keys_.find(canonical_key(args[0].to_string())) != pressed_keys_.end());
    }
    if (name == "mouse_held") {
        require(1);
        const auto found = button_refs_.find(lower_ascii(args[0].to_string()));
        return Value::of_bool(found != button_refs_.end() && found->second > 0);
    }
    if (name == "suspend_hotkeys") {
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
    if (name == "hotkeys_suspended") {
        require(0);
        return Value::of_bool(suspended_hotkeys_);
    }
    if (name == "set_hotkey_enabled") {
        require(2);
        const std::string id = hotkey_id_for_name(args[0].to_string());
        if (args[1].truthy()) {
            disabled_hotkeys_.erase(id);
        } else {
            disabled_hotkeys_.insert(id);
        }
        return Value::of_bool(true);
    }
    if (name == "hotkey_enabled") {
        require(1);
        const std::string id = hotkey_id_for_name(args[0].to_string());
        const bool disabled = disabled_hotkeys_.find(id) != disabled_hotkeys_.end();
        const bool suspended = suspended_hotkeys_ &&
                               suspended_exempt_hotkeys_.find(id) == suspended_exempt_hotkeys_.end();
        return Value::of_bool(!disabled && !suspended);
    }
    const auto find_gui_widget = [&](const std::string& raw_id) -> GuiHost* {
        const std::string id = lower_ascii(trim_ascii(raw_id));
        for (auto& host : gui_hosts_) {
            if (host->has_widget(id)) return host.get();
        }
        return nullptr;
    };
    if (name == "gui_value") {
        require(1);
        const std::string id = lower_ascii(trim_ascii(args[0].to_string()));
        GuiHost* host = find_gui_widget(id);
        if (host == nullptr) {
            throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is not declared in a gui block.");
        }
        return Value::of_str(host->value(id));
    }
    if (name == "gui_set_text") {
        require(2);
        const std::string id = lower_ascii(trim_ascii(args[0].to_string()));
        GuiHost* host = find_gui_widget(id);
        if (host == nullptr) {
            throw RuntimeErrorKeyloom("GUI widget " + py_repr_string(id) + " is not declared in a gui block.");
        }
        host->set_text(id, args[1].to_string());
        return Value::of_bool(true);
    }
    if (name == "gui_set_visible") {
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
            throw RuntimeErrorKeyloom("GUI panel " + py_repr_string(title) + " is not declared in a gui block.");
        }
        return Value::of_bool(true);
    }
    if (name == "active_window_title") {
        require(0);
        return Value::of_str(active_window_info().at("title"));
    }
    if (name == "active_window_class") {
        require(0);
        return Value::of_str(active_window_info().at("class"));
    }
    if (name == "active_window_process") {
        require(0);
        return Value::of_str(active_window_info().at("process"));
    }
    if (name == "active_window_pid") {
        require(0);
        const std::string value = active_window_info().at("pid");
        if (value.empty()) return Value::of_int(0);
        try {
            return Value::of_int(std::stoll(value));
        } catch (const std::exception&) {
            return Value::of_int(0);
        }
    }
    if (name == "gui_alert") {
        if (args.size() < 1 || args.size() > 2) {
            throw RuntimeErrorKeyloom("Invalid arguments for " + name + "(): expected 1 or 2 argument(s), got " +
                                      std::to_string(args.size()));
        }
        gui_alert(args[0].to_string(), args.size() == 2 ? args[1].to_string() : script_.name);
        return Value::of_bool(true);
    }
    if (name == "gui_input") {
        if (args.size() < 1 || args.size() > 3) {
            throw RuntimeErrorKeyloom("Invalid arguments for " + name + "(): expected 1 to 3 argument(s), got " +
                                      std::to_string(args.size()));
        }
        return Value::of_str(gui_input(args[0].to_string(),
                                       args.size() > 1 ? args[1].to_string() : std::string(),
                                       args.size() > 2 ? args[2].to_string() : script_.name));
    }
    if (name == "gui_confirm") {
        if (args.size() < 1 || args.size() > 2) {
            throw RuntimeErrorKeyloom("Invalid arguments for " + name + "(): expected 1 or 2 argument(s), got " +
                                      std::to_string(args.size()));
        }
        return Value::of_bool(gui_confirm(args[0].to_string(),
                                           args.size() == 2 ? args[1].to_string() : script_.name));
    }
    if (name == "screen_width") {
        require(0);
        return Value::of_int(width_);
    }
    if (name == "screen_height") {
        require(0);
        return Value::of_int(height_);
    }

    // Strings.
    const auto require_string = [&](size_t index) -> const std::string& {
        if (args[index].type != Value::Type::Str) {
            throw RuntimeErrorKeyloom(name + "() expects a string, got " + py_type_name(args[index]));
        }
        return args[index].text;
    };
    if (name == "upper") {
        require(1);
        std::string text = require_string(0);
        for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return Value::of_str(std::move(text));
    }
    if (name == "lower") {
        require(1);
        std::string text = require_string(0);
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return Value::of_str(std::move(text));
    }
    if (name == "trim") {
        require(1);
        return Value::of_str(trim_ascii(require_string(0)));
    }
    if (name == "starts_with") {
        require(2);
        const std::string& text = require_string(0);
        const std::string& prefix = require_string(1);
        return Value::of_bool(text.rfind(prefix, 0) == 0);
    }
    if (name == "ends_with") {
        require(2);
        const std::string& text = require_string(0);
        const std::string& suffix = require_string(1);
        if (suffix.size() > text.size()) return Value::of_bool(false);
        return Value::of_bool(text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0);
    }
    if (name == "contains") {
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
    if (name == "replace") {
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
    if (name == "substr") {
        if (args.size() < 2 || args.size() > 3) {
            throw RuntimeErrorKeyloom("Invalid arguments for substr(): expected 2 or 3 argument(s), got " +
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
    if (name == "index_of") {
        require(2);
        const std::string& text = require_string(0);
        const std::string& needle = require_string(1);
        const size_t position = text.find(needle);
        if (position == std::string::npos) return Value::of_int(-1);
        return Value::of_int(static_cast<long long>(utf8_length(text.substr(0, position))));
    }

    // Lists.
    if (name == "join") {
        require(2);
        const std::string& separator = require_string(1);
        std::string out;
        const std::vector<Value>& items = args[0].list_items();
        for (size_t index = 0; index < items.size(); ++index) {
            if (index > 0) out += separator;
            if (items[index].type != Value::Type::Str) {
                throw RuntimeErrorKeyloom(std::string("join() expects a list of strings, got ") + py_type_name(items[index]));
            }
            out += items[index].text;
        }
        return Value::of_str(std::move(out));
    }
    if (name == "split") {
        require(2);
        const std::string& text = require_string(0);
        const std::string& separator = require_string(1);
        if (separator.empty()) throw RuntimeErrorKeyloom("split() separator cannot be empty");
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
    if (name == "append") {
        require(2);
        std::vector<Value>& items = args[0].list_items();
        items.push_back(args[1]);
        return args[0];
    }
    if (name == "sort") {
        require(1);
        std::vector<Value>& items = args[0].list_items();
        std::stable_sort(items.begin(), items.end(),
                         [](const Value& left, const Value& right) { return compare_values(left, right) < 0; });
        return args[0];
    }

    if (is_builtin_function(name)) {
        throw RuntimeErrorKeyloom("Function " + py_repr_string(name) + " is declared but not implemented");
    }
    throw RuntimeErrorKeyloom("Unknown function " + py_repr_string(name));
}

Task Runtime::eval(const ExprPtr& expr, const EventContext& context, Value& out) {
    switch (expr->kind) {
        case Expr::Kind::Literal:
            out = expr->literal;
            break;
        case Expr::Kind::Variable: {
            if (expr->name == "event_key") {
                out = Value::of_str(context.key);
                break;
            }
            if (expr->name == "event_edge") {
                out = Value::of_str(context.edge);
                break;
            }
            Value value;
            if (!lookup_variable(expr->name, value)) {
                throw RuntimeErrorKeyloom("Unknown variable " + py_repr_string(expr->name));
            }
            out = std::move(value);
            break;
        }
        case Expr::Kind::List: {
            std::vector<Value> values;
            values.reserve(expr->args.size());
            for (const auto& argument : expr->args) {
                Value value;
                co_await eval(argument, context, value);
                values.push_back(std::move(value));
            }
            out = Value::of_list(std::move(values));
            break;
        }
        case Expr::Kind::Index: {
            Value base;
            Value index_value;
            co_await eval(expr->args.at(0), context, base);
            co_await eval(expr->args.at(1), context, index_value);
            if (!base.is_list()) {
                throw RuntimeErrorKeyloom(std::string("only a list can be indexed, got ") + py_type_name(base));
            }
            const std::vector<Value>& items = base.list_items();
            long long index = index_value.as_int();
            if (index < 0) index += static_cast<long long>(items.size());
            if (index < 0 || index >= static_cast<long long>(items.size())) {
                throw RuntimeErrorKeyloom("list index out of range");
            }
            out = items[static_cast<size_t>(index)];
            break;
        }
        case Expr::Kind::Unary: {
            Value value;
            co_await eval(expr->args.at(0), context, value);
            if (expr->name == "-") {
                if (value.type == Value::Type::Float) {
                    out = Value::of_float(-value.real);
                } else {
                    out = Value::of_int(-value.as_int());
                }
                break;
            }
            if (expr->name == "+") {
                out = value;
                break;
            }
            out = Value::of_bool(!value.truthy());
            break;
        }
        case Expr::Kind::Binary: {
            const std::string& op = expr->name;
            if (op == "and" || op == "&&" || op == "or" || op == "||") {
                const bool is_and = op == "and" || op == "&&";
                Value left;
                co_await eval(expr->args.at(0), context, left);
                if (is_and ? !left.truthy() : left.truthy()) {
                    out = Value::of_bool(!is_and);
                    break;
                }
                Value right;
                co_await eval(expr->args.at(1), context, right);
                out = Value::of_bool(right.truthy());
                break;
            }

            Value left;
            Value right;
            co_await eval(expr->args.at(0), context, left);
            co_await eval(expr->args.at(1), context, right);

            if (op == "==") {
                out = Value::of_bool(value_equals(left, right));
                break;
            }
            if (op == "!=") {
                out = Value::of_bool(!value_equals(left, right));
                break;
            }

            if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                const int comparison = compare_values(left, right);
                if (op == "<") {
                    out = Value::of_bool(comparison < 0);
                } else if (op == "<=") {
                    out = Value::of_bool(comparison <= 0);
                } else if (op == ">") {
                    out = Value::of_bool(comparison > 0);
                } else {
                    out = Value::of_bool(comparison >= 0);
                }
                break;
            }

            if (op == "+" && left.type == Value::Type::Str && right.type == Value::Type::Str) {
                out = Value::of_str(left.text + right.text);
                break;
            }
            if (op == "+" && left.is_list() && right.is_list()) {
                std::vector<Value> joined = left.list_items();
                for (const Value& item : right.list_items()) joined.push_back(item);
                out = Value::of_list(std::move(joined));
                break;
            }
            if (!numeric_like(left) || !numeric_like(right)) {
                throw RuntimeErrorKeyloom("unsupported operand type(s) for " + op + ": '" +
                                          py_type_name(left) + "' and '" + py_type_name(right) + "'");
            }

            const bool want_float = left.type == Value::Type::Float || right.type == Value::Type::Float;
            if (op == "/") {
                const double divisor = right.as_double();
                if (divisor == 0.0) throw RuntimeErrorKeyloom("Division by zero");
                out = Value::of_float(left.as_double() / divisor);
                break;
            }
            if (op == "%") {
                if (!want_float) {
                    const long long divisor = right.as_int();
                    if (divisor == 0) throw RuntimeErrorKeyloom("Division by zero");
                    const long long value = left.as_int();
                    long long result = value % divisor;
                    if (result != 0 && ((result < 0) != (divisor < 0))) result += divisor;
                    out = Value::of_int(result);
                    break;
                }
                const double divisor = right.as_double();
                if (divisor == 0.0) throw RuntimeErrorKeyloom("Division by zero");
                double result = std::fmod(left.as_double(), divisor);
                if (result != 0.0 && ((result < 0.0) != (divisor < 0.0))) result += divisor;
                out = Value::of_float(result);
                break;
            }
            if (op == "+") {
                out = want_float ? Value::of_float(left.as_double() + right.as_double())
                                 : Value::of_int(left.as_int() + right.as_int());
                break;
            }
            if (op == "-") {
                out = want_float ? Value::of_float(left.as_double() - right.as_double())
                                 : Value::of_int(left.as_int() - right.as_int());
                break;
            }
            if (op == "*") {
                out = want_float ? Value::of_float(left.as_double() * right.as_double())
                                 : Value::of_int(left.as_int() * right.as_int());
                break;
            }
            throw RuntimeErrorKeyloom("unsupported operator " + py_repr_string(op));
        }
        case Expr::Kind::Call: {
            std::vector<Value> args;
            args.reserve(expr->args.size());
            for (const auto& argument : expr->args) {
                Value value;
                co_await eval(argument, context, value);
                args.push_back(std::move(value));
            }
            const auto user_function = functions_.find(expr->name);
            if (user_function != functions_.end()) {
                Value result;
                co_await call_user_function(*user_function->second, std::move(args), context, result);
                out = std::move(result);
                break;
            }
            out = call_function(expr->name, std::move(args), context);
            break;
        }
    }
}

void Runtime::hold_button(const std::string& owner, const std::string& button) {
    const auto existing = held_tokens_.find(owner);
    if (existing != held_tokens_.end()) {
        if (existing->second != button) {
            throw RuntimeErrorKeyloom("Hold token " + py_repr_string(owner) + " already belongs to " +
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

}  // namespace keyloom
