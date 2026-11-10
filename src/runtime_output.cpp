#include "runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <unordered_map>

#include "errors.hpp"
#include "keys.hpp"
#include "runtime_internal.hpp"
#include "subprocess.hpp"


namespace keyloom {
namespace {

using detail::lower_ascii;
using detail::split_on;
using detail::trim_ascii;

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

}  // namespace


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

}  // namespace keyloom
