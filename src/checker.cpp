#include "checker.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "builtins.hpp"
#include "errors.hpp"
#include "keys.hpp"

namespace loomwright {
namespace {

struct OptionRule {
    std::string_view name;
    std::span<const std::string_view> values;  // empty means "any string"
    bool boolean = false;
};

namespace {
constexpr std::array<std::string_view, 2> kInputValues = {"auto", "evdev"};
constexpr std::array<std::string_view, 3> kOutputValues = {"auto", "ydotool", "uinput"};
constexpr std::array<std::string_view, 3> kCompositorValues = {"auto", "x11", "wayland"};
constexpr std::array<OptionRule, 6> kOptionRules = {{
    {"input", kInputValues, false},
    {"output", kOutputValues, false},
    {"compositor", kCompositorValues, false},
    {"panic", {}, false},
    {"screen", {}, false},
    {"allow_launch", {}, true},
}};
}  // namespace

const std::span<const OptionRule> option_rules() { return kOptionRules; }

const OptionRule* find_rule(std::string_view key) {
    for (const auto& rule : option_rules()) {
        if (key == rule.name) return &rule;
    }
    return nullptr;
}

bool uinput_writable() { return access("/dev/uinput", W_OK) == 0; }

template <std::invocable<const Node&> F>
void walk(const std::vector<Node>& nodes, F&& visit) {
    for (const auto& node : nodes) {
        visit(node);
        walk(node.body, visit);
        walk(node.otherwise, visit);
    }
}

/// How deeply a statement sits inside loops and functions.
struct Scope {
    int loops = 0;
    int functions = 0;
};

/// Walk statements with the loop and function depth of every node.
template <std::invocable<const Node&, const Scope&> F>
void walk_scoped(const std::vector<Node>& nodes, Scope scope, F&& visit) {
    for (const auto& node : nodes) {
        Scope inner = scope;
        if (node.stmt == StmtKind::For || node.stmt == StmtKind::Repeat || node.stmt == StmtKind::While) {
            ++inner.loops;
        }
        visit(node, inner);
        walk_scoped(node.body, inner, visit);
        walk_scoped(node.otherwise, inner, visit);
    }
}

template <std::invocable<const Expr&> F>
void walk_expression(const ExprPtr& expr, F&& visit) {
    if (!expr) return;
    visit(*expr);
    for (const auto& argument : expr->args) walk_expression(argument, visit);
}

std::string canonical_or_empty(const std::string& name) {
    try {
        return canonical_key_name(name);
    } catch (const BackendError&) {
        return std::string();
    }
}

/// "1" or "2 to 3", for a function that accepts a range of argument counts.
std::string arity_text(const BuiltinInfo& function) {
    if (function.max_args < 0) return std::to_string(function.min_args) + " or more";
    if (function.min_args == function.max_args) return std::to_string(function.min_args);
    return std::to_string(function.min_args) + " to " + std::to_string(function.max_args);
}

/// "CTRL+ALT+T", for a diagnostic about a chord binding.
std::string join_chord(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += "+";
        out += part;
    }
    return out;
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
            out.push_back({"error", "Unknown option " + quote_string(key), 0});
        } else if (rule->boolean) {
            // A boolean option also accepts the integers 0 and 1.
            const bool numeric_flag = value.type == Value::Type::Bool ||
                                      (value.type == Value::Type::Int && (value.integer == 0 || value.integer == 1));
            if (!numeric_flag) {
                out.push_back({"error", "Invalid value " + value.repr() + " for option " + quote_string(key), 0});
            }
        } else if (!rule->values.empty()) {
            const std::string text = value.type == Value::Type::Str ? value.text : value.repr();
            const bool known = value.type == Value::Type::Str &&
                               std::find(rule->values.begin(), rule->values.end(), text) != rule->values.end();
            if (!known) {
                out.push_back({"error", "Invalid value " + value.repr() + " for option " + quote_string(key), 0});
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

    std::map<std::string, int> remap_sources;
    for (const auto& remap : script.remaps) {
        try {
            remap_sources[canonical_key_name(remap.source)] = remap.line;
        } catch (const BackendError&) {
            // Canonicalization cannot fail here because validate_key_name below reports it.
        }
    }

    std::map<std::string, std::string> gui_widgets;
    std::set<std::string> gui_titles;
    for (const auto& panel : script.panels) {
        if (!gui_titles.insert(panel.title).second) {
            out.push_back({"error", "GUI panel title " + quote_string(panel.title) + " is used more than once", panel.line});
        }
        if (panel.widgets.empty()) {
            out.push_back({"error", "GUI panel " + quote_string(panel.title) + " has no widgets", panel.line});
        }
        for (const auto& widget : panel.widgets) {
            if (widget.id.empty()) continue;
            const std::string kind = widget.kind == GuiWidget::Kind::Input ? "input" : "button";
            if (!gui_widgets.emplace(widget.id, kind).second) {
                out.push_back({"error", "GUI widget " + quote_string(widget.id) + " is declared more than once", widget.line});
            }
        }
    }

    std::map<std::string, const Function*> functions;
    for (const auto& function : script.functions) {
        if (is_builtin_function(function.name)) {
            out.push_back({"error", "Function " + quote_string(function.name) + " shadows a built-in function",
                           function.line});
        }
        if (!functions.emplace(function.name, &function).second) {
            out.push_back({"error", "Function " + quote_string(function.name) + " is declared more than once",
                           function.line});
        }
        std::set<std::string> params;
        for (const auto& param : function.params) {
            if (!params.insert(param).second) {
                out.push_back({"error", "Parameter " + quote_string(param) + " is declared more than once in function " +
                                           quote_string(function.name),
                               function.line});
            }
        }
    }

    std::set<std::string> held_tokens;
    std::set<std::string> freed_tokens;

    for (const auto& remap : script.remaps) {
        std::string source;
        try {
            source = validate_key_name(remap.source);
        } catch (const BackendError& error) {
            out.push_back({"error", error.what(), remap.line});
            source = canonical_key_name(remap.source);
        }
        std::string target = remap.target;
        std::set<std::string> chord;
        size_t start = 0;
        while (true) {
            const size_t plus = target.find('+', start);
            const std::string part =
                plus == std::string::npos ? target.substr(start) : target.substr(start, plus - start);
            try {
                const std::string canonical = validate_key_name(part);
                if (!chord.insert(canonical).second) {
                    out.push_back({"error", "Key chord in remap target contains " + quote_string(canonical) + " more than once", remap.line});
                }
            } catch (const BackendError& error) {
                out.push_back({"error", error.what(), remap.line});
                break;
            }
            if (plus == std::string::npos) break;
            start = plus + 1;
        }

        auto duplicate = remap_sources.find(source);
        if (duplicate != remap_sources.end() && duplicate->second != remap.line) {
            out.push_back({"error", "Remap source " + source + " is declared more than once", remap.line});
        }
        if (have_panic_key && source == panic_key) {
            out.push_back({"error", "Remap source " + source + " is the panic key", remap.line});
        }
        bool bound_by_event = false;
        bool explicitly_consumed = false;
        for (const auto& event : script.events) {
            if (event.event_kind != "key") continue;
            if (canonical_key_name(event.event_key) == source) {
                bound_by_event = true;
                if (event.edge != "up") break;
            }
            walk(event.body, [&](const Node& node) {
                if (node.stmt == StmtKind::Consume && canonical_or_empty(node.a) == source) explicitly_consumed = true;
            });
        }
        if (bound_by_event) {
            out.push_back({"error", "Remap source " + source + " is already bound by a when key block", remap.line});
        }
        if (explicitly_consumed) {
            out.push_back({"warning", "Remap source " + source + " is also consumed explicitly; the remap is already consumed", remap.line});
        }
    }

    for (const auto& event : script.events) {
        const std::string& kind = event.event_kind;
        if (kind == "key") {
            // A plain binding validates its single key; a chord validates every
            // key of the chord.
            const std::vector<std::string> parts =
                event.chord_parts.empty() ? std::vector<std::string>{event.event_key} : event.chord_parts;
            for (const auto& part : parts) {
                try {
                    validate_key_name(part);
                } catch (const BackendError& error) {
                    out.push_back({"error", error.what(), event.line});
                }
            }
            if (have_panic_key && canonical_or_empty(event.event_key) == panic_key) {
                out.push_back({"warning",
                               "Binding " + event.event_key + " is unreachable while it is the panic key",
                               event.line});
            }
            // A modifier that is the panic key stops the runtime before the
            // primary key can go down, so the chord never fires.
            for (size_t index = 0; index + 1 < parts.size(); ++index) {
                if (have_panic_key && canonical_or_empty(parts[index]) == panic_key) {
                    out.push_back({"warning",
                                   "Chord binding " + quote_string(join_chord(parts)) +
                                       " can never fire while " + parts[index] + " is the panic key",
                                   event.line});
                }
            }
        }
        if (kind == "mouse" && !is_mouse_button(event.event_key)) {
            out.push_back({"error", "Unknown mouse button " + quote_string(event.event_key), event.line});
        }
        if (kind == "gui") {
            if (event.edge == "clicked") {
                const auto found = gui_widgets.find(event.event_key);
                if (found == gui_widgets.end()) {
                    out.push_back({"error", "GUI widget " + quote_string(event.event_key) + " is not declared in a gui block", event.line});
                } else if (found->second != "button") {
                    out.push_back({"error", "GUI widget " + quote_string(event.event_key) + " is not a button", event.line});
                }
            } else if (event.edge == "close") {
                if (event.event_key != "window") {
                    out.push_back({"error", "GUI window close event must be 'when gui window closes'", event.line});
                }
            } else {
                out.push_back({"error", "Unsupported GUI event edge " + quote_string(event.edge), event.line});
            }
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
                    out.push_back({"error", "Unknown mouse button " + quote_string(node.a), node.line});
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
    if (!script.remaps.empty()) needs_passthrough = true;

    // Function calls, loop control and `return` placement.
    const auto check_statements = [&](const std::vector<Node>& nodes, Scope scope) {
        walk_scoped(nodes, scope, [&](const Node& node, const Scope& inner) {
            if (node.stmt == StmtKind::Break && inner.loops == 0) {
                out.push_back({"error", "break is only allowed inside a loop", node.line});
            }
            if (node.stmt == StmtKind::Continue && inner.loops == 0) {
                out.push_back({"error", "continue is only allowed inside a loop", node.line});
            }
            if (node.stmt == StmtKind::Return && inner.functions == 0) {
                out.push_back({"error", "return is only allowed inside a function", node.line});
            }
            for (const auto& expr : node.exprs) {
                walk_expression(expr, [&](const Expr& expression) {
                    if (expression.kind != Expr::Kind::Call) return;
                    const std::string& name = expression.name;
                    const size_t count = expression.args.size();
                    const BuiltinInfo* builtin = find_builtin_function(name);
                    if (builtin != nullptr) {
                        const bool fits = count >= static_cast<size_t>(builtin->min_args) &&
                                          (builtin->max_args < 0 ||
                                           count <= static_cast<size_t>(builtin->max_args));
                        if (!fits) {
                            out.push_back({"error",
                                           "Function " + name + "() takes " + arity_text(*builtin) +
                                               " argument(s), got " + std::to_string(count),
                                           expression.line});
                        }
                        return;
                    }
                    const auto found = functions.find(name);
                    if (found == functions.end()) {
                        out.push_back({"error", "Unknown function " + quote_string(name), expression.line});
                        return;
                    }
                    if (count != found->second->params.size()) {
                        out.push_back({"error",
                                       "Function " + name + "() takes " + std::to_string(found->second->params.size()) +
                                           " argument(s), got " + std::to_string(count),
                                       expression.line});
                    }
                });
            }
        });
    };

    for (const auto& event : script.events) check_statements(event.body, Scope{});
    for (const auto& function : script.functions) {
        Scope scope;
        scope.functions = 1;
        check_statements(function.body, scope);
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
    std::ranges::sort(unheld);
    for (const auto& token : unheld) {
        out.push_back({"warning",
                       "Mouse hold token " + quote_string(token) +
                           " is never freed; runtime safety cleanup will release it",
                       0});
    }
    std::vector<std::string> unfreed;
    for (const auto& token : freed_tokens) {
        if (held_tokens.find(token) == held_tokens.end()) unfreed.push_back(token);
    }
    std::ranges::sort(unfreed);
    for (const auto& token : unfreed) {
        out.push_back({"warning", "Mouse hold token " + quote_string(token) + " is freed but never held", 0});
    }

    if (out.empty()) out.push_back({"ok", "Script syntax and static checks passed", 0});
    return out;
}

}  // namespace loomwright
