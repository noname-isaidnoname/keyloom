#include "checker.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "errors.hpp"
#include "keys.hpp"

namespace keyloom {
namespace {

struct OptionRule {
    const char* name;
    std::vector<std::string> values;  // empty means "any string"
    bool boolean = false;
};

const std::vector<OptionRule>& option_rules() {
    static const std::vector<OptionRule> rules = {
        {"input", {"auto", "evdev"}, false},
        {"output", {"auto", "ydotool", "uinput"}, false},
        {"panic", {}, false},
        {"screen", {}, false},
        {"allow_launch", {}, true},
    };
    return rules;
}

const OptionRule* find_rule(const std::string& key) {
    for (const auto& rule : option_rules()) {
        if (key == rule.name) return &rule;
    }
    return nullptr;
}

bool uinput_writable() { return access("/dev/uinput", W_OK) == 0; }

void walk(const std::vector<Node>& nodes, const std::function<void(const Node&)>& visit) {
    for (const auto& node : nodes) {
        visit(node);
        walk(node.body, visit);
        walk(node.otherwise, visit);
    }
}

std::string canonical_or_empty(const std::string& name) {
    try {
        return canonical_key_name(name);
    } catch (const BackendError&) {
        return std::string();
    }
}

}  // namespace

std::string Diagnostic::to_string() const {
    std::string out = level + ": ";
    if (line > 0) out += "line " + std::to_string(line) + ": ";
    out += message;
    return out;
}

std::vector<Diagnostic> check_script(const Script& script) {
    std::vector<Diagnostic> out;

    for (const auto& entry : script.options) {
        const std::string& key = entry.first;
        const Value& value = entry.second;
        const OptionRule* rule = find_rule(key);
        if (rule == nullptr) {
            out.push_back({"error", "Unknown option " + py_repr_string(key), 0});
        } else if (rule->boolean) {
            // Python's set membership accepted 0/1 for the boolean option too.
            const bool numeric_flag = value.type == Value::Type::Bool ||
                                      (value.type == Value::Type::Int && (value.integer == 0 || value.integer == 1));
            if (!numeric_flag) {
                out.push_back({"error", "Invalid value " + value.repr() + " for option " + py_repr_string(key), 0});
            }
        } else if (!rule->values.empty()) {
            const std::string text = value.type == Value::Type::Str ? value.text : value.repr();
            const bool known = value.type == Value::Type::Str &&
                               std::find(rule->values.begin(), rule->values.end(), text) != rule->values.end();
            if (!known) {
                out.push_back({"error", "Invalid value " + value.repr() + " for option " + py_repr_string(key), 0});
            }
        }
    }

    std::string panic_key;
    bool have_panic_key = false;
    if (const Value* panic = script.option("panic")) {
        if (panic->type != Value::Type::Str) {
            out.push_back({"error", "Option 'panic' must be a key name string or \"none\"", 0});
        } else {
            std::string lowered;
            for (char c : panic->text) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (lowered != "none") {
                try {
                    panic_key = validate_key_name(panic->text);
                    have_panic_key = true;
                } catch (const BackendError& error) {
                    out.push_back({"error", error.what(), 0});
                }
            }
        }
    }
    if (const Value* screen = script.option("screen")) {
        if (screen->type != Value::Type::Str) {
            out.push_back({"error", "Option 'screen' must be 'auto' or a string such as '1920x1080'", 0});
        }
    }

    if (!have_panic_key && script.option("panic") == nullptr) {
        panic_key = canonical_key_name("PAUSE");
        have_panic_key = true;
    }

    std::set<std::string> held_tokens;
    std::set<std::string> freed_tokens;

    for (const auto& event : script.events) {
        const std::string& kind = event.event_kind;
        if (kind == "key") {
            try {
                validate_key_name(event.event_key);
            } catch (const BackendError& error) {
                out.push_back({"error", error.what(), event.line});
            }
            if (have_panic_key && canonical_or_empty(event.event_key) == panic_key) {
                out.push_back({"warning",
                               "Binding " + event.event_key + " is unreachable while it is the panic key",
                               event.line});
            }
        }
        if (kind == "mouse" && !is_mouse_button(event.event_key)) {
            out.push_back({"error", "Unknown mouse button " + py_repr_string(event.event_key), event.line});
        }
        if (kind == "timer" && event.interval <= 0.0) {
            out.push_back({"error", "Timer interval must be greater than zero", event.line});
        }
        if ((kind == "key" || kind == "mouse") && event.edge == "held" && event.interval <= 0.0) {
            out.push_back({"error", "Held-event interval must be greater than zero", event.line});
        }

        walk(event.body, [&](const Node& node) {
            if (node.stmt == StmtKind::Mouse && !node.is_event) {
                if (!is_mouse_button(node.a)) {
                    out.push_back({"error", "Unknown mouse button " + py_repr_string(node.a), node.line});
                }
                const std::string owner =
                    (kind == "key" || kind == "mouse") ? event.event_key : std::string("script");
                const std::string default_token = "default:" + owner + ":" + node.a;
                const std::string token = node.c.empty() ? default_token : node.c;
                if (node.b == "hold") held_tokens.insert(token);
                if (node.b == "free") freed_tokens.insert(token);
            } else if (node.stmt == StmtKind::Key && !node.is_event) {
                size_t start = 0;
                while (true) {
                    const size_t plus = node.a.find('+', start);
                    const std::string part =
                        plus == std::string::npos ? node.a.substr(start) : node.a.substr(start, plus - start);
                    try {
                        validate_key_name(part);
                    } catch (const BackendError& error) {
                        out.push_back({"error", error.what(), node.line});
                        break;
                    }
                    if (plus == std::string::npos) break;
                    start = plus + 1;
                }
            } else if (node.stmt == StmtKind::Consume && !node.is_event) {
                try {
                    validate_key_name(node.a);
                } catch (const BackendError& error) {
                    out.push_back({"error", error.what(), node.line});
                }
                if (have_panic_key && canonical_or_empty(node.a) == panic_key) {
                    out.push_back({"warning",
                                   "Key " + node.a +
                                       " is the panic key and is never consumed; it always passes through",
                                   node.line});
                }
            }
        });
    }

    bool needs_pointer = false;
    bool needs_passthrough = false;
    for (const auto& event : script.events) {
        walk(event.body, [&](const Node& node) {
            if (node.stmt == StmtKind::Cursor || node.stmt == StmtKind::Scroll) needs_pointer = true;
            if (node.stmt == StmtKind::Consume) needs_passthrough = true;
        });
    }

    if (needs_pointer && !uinput_writable()) {
        out.push_back({"warning",
                       "cursor motion and scrolling require write access to /dev/uinput on this machine",
                       0});
    }
    if (needs_passthrough && !uinput_writable()) {
        out.push_back({"warning", "key consumption needs write access to /dev/uinput to replay non-consumed input", 0});
    }

    std::vector<std::string> unheld;
    for (const auto& token : held_tokens) {
        if (freed_tokens.find(token) == freed_tokens.end()) unheld.push_back(token);
    }
    std::sort(unheld.begin(), unheld.end());
    for (const auto& token : unheld) {
        out.push_back({"warning",
                       "Mouse hold token " + py_repr_string(token) +
                           " is never freed; runtime safety cleanup will release it",
                       0});
    }
    std::vector<std::string> unfreed;
    for (const auto& token : freed_tokens) {
        if (held_tokens.find(token) == held_tokens.end()) unfreed.push_back(token);
    }
    std::sort(unfreed.begin(), unfreed.end());
    for (const auto& token : unfreed) {
        out.push_back({"warning", "Mouse hold token " + py_repr_string(token) + " is freed but never held", 0});
    }

    if (out.empty()) out.push_back({"ok", "Script syntax and static checks passed", 0});
    return out;
}

}  // namespace keyloom
