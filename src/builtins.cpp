#include "builtins.hpp"

namespace loomwright {
namespace {

/// The table is the single source of truth for which functions exist and how
/// many arguments they accept. `Runtime::call_function` implements exactly
/// these names; anything else is an unknown function.
const std::vector<BuiltinInfo>& table() {
    static const std::vector<BuiltinInfo> functions = {
        // Numbers and general helpers.
        {"min", 1, -1},
        {"max", 1, -1},
        {"abs", 1, 1},
        {"round", 1, 1},
        {"floor", 1, 1},
        {"ceil", 1, 1},
        {"sqrt", 1, 1},
        {"clamp", 3, 3},
        {"int", 1, 1},
        {"float", 1, 1},
        {"is_number", 1, 1},
        {"str", 1, 1},
        {"bool", 1, 1},
        {"len", 1, 1},
        {"random", 0, 0},
        {"randint", 2, 2},
        {"elapsed", 0, 0},
        // Strings.
        {"upper", 1, 1},
        {"lower", 1, 1},
        {"trim", 1, 1},
        {"starts_with", 2, 2},
        {"ends_with", 2, 2},
        {"contains", 2, 2},
        {"replace", 3, 3},
        {"substr", 2, 3},
        {"index_of", 2, 2},
        // Lists.
        {"join", 2, 2},
        {"split", 2, 2},
        {"append", 2, 2},
        {"sort", 1, 1},
        // Runtime state.
        {"key_down", 1, 1},
        {"mouse_held", 1, 1},
        {"suspend_hotkeys", 1, 1},
        {"hotkeys_suspended", 0, 0},
        {"set_hotkey_enabled", 2, 2},
        {"hotkey_enabled", 1, 1},
        {"screen_width", 0, 0},
        {"screen_height", 0, 0},
        // Windows and dialogs.
        {"active_window_title", 0, 0},
        {"active_window_class", 0, 0},
        {"active_window_process", 0, 0},
        {"active_window_pid", 0, 0},
        {"gui_alert", 1, 2},
        {"gui_input", 1, 3},
        {"gui_confirm", 1, 2},
        // Panels.
        {"gui_value", 1, 1},
        {"gui_set_text", 2, 2},
        {"gui_set_visible", 2, 2},
    };
    return functions;
}

}  // namespace

const std::vector<BuiltinInfo>& builtin_functions() { return table(); }

const BuiltinInfo* find_builtin_function(const std::string& name) {
    for (const auto& function : table()) {
        if (function.name == name) return &function;
    }
    return nullptr;
}

bool is_builtin_function(const std::string& name) { return find_builtin_function(name) != nullptr; }

}  // namespace loomwright
