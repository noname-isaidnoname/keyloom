#include "builtins.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace loomwright {
namespace {

/// The table is the single source of truth for which functions exist and how
/// many arguments they accept. `Runtime::call_builtin` implements exactly
/// these ids; anything else is a user-defined function or an error reported
/// by the checker. Sorted by name for the binary search; static_asserts guard
/// the invariant.
constexpr std::array<BuiltinInfo, 48> kBuiltins = {{
    {"abs", Builtin::Abs, 1, 1},
    {"active_window_class", Builtin::ActiveWindowClass, 0, 0},
    {"active_window_pid", Builtin::ActiveWindowPid, 0, 0},
    {"active_window_process", Builtin::ActiveWindowProcess, 0, 0},
    {"active_window_title", Builtin::ActiveWindowTitle, 0, 0},
    {"append", Builtin::Append, 2, 2},
    {"bool", Builtin::Bool, 1, 1},
    {"ceil", Builtin::Ceil, 1, 1},
    {"clamp", Builtin::Clamp, 3, 3},
    {"contains", Builtin::Contains, 2, 2},
    {"elapsed", Builtin::Elapsed, 0, 0},
    {"ends_with", Builtin::EndsWith, 2, 2},
    {"float", Builtin::Float, 1, 1},
    {"floor", Builtin::Floor, 1, 1},
    {"gui_alert", Builtin::GuiAlert, 1, 2},
    {"gui_confirm", Builtin::GuiConfirm, 1, 2},
    {"gui_input", Builtin::GuiInput, 1, 3},
    {"gui_set_text", Builtin::GuiSetText, 2, 2},
    {"gui_set_visible", Builtin::GuiSetVisible, 2, 2},
    {"gui_value", Builtin::GuiValue, 1, 1},
    {"hotkey_enabled", Builtin::HotkeyEnabled, 1, 1},
    {"hotkeys_suspended", Builtin::HotkeysSuspended, 0, 0},
    {"index_of", Builtin::IndexOf, 2, 2},
    {"int", Builtin::Int, 1, 1},
    {"is_number", Builtin::IsNumber, 1, 1},
    {"join", Builtin::Join, 2, 2},
    {"key_down", Builtin::KeyDown, 1, 1},
    {"len", Builtin::Len, 1, 1},
    {"lower", Builtin::Lower, 1, 1},
    {"max", Builtin::Max, 1, -1},
    {"min", Builtin::Min, 1, -1},
    {"mouse_held", Builtin::MouseHeld, 1, 1},
    {"randint", Builtin::Randint, 2, 2},
    {"random", Builtin::Random, 0, 0},
    {"replace", Builtin::Replace, 3, 3},
    {"round", Builtin::Round, 1, 1},
    {"screen_height", Builtin::ScreenHeight, 0, 0},
    {"screen_width", Builtin::ScreenWidth, 0, 0},
    {"set_hotkey_enabled", Builtin::SetHotkeyEnabled, 2, 2},
    {"sort", Builtin::Sort, 1, 1},
    {"split", Builtin::Split, 2, 2},
    {"sqrt", Builtin::Sqrt, 1, 1},
    {"starts_with", Builtin::StartsWith, 2, 2},
    {"str", Builtin::Str, 1, 1},
    {"substr", Builtin::Substr, 2, 3},
    {"suspend_hotkeys", Builtin::SuspendHotkeys, 1, 1},
    {"trim", Builtin::Trim, 1, 1},
    {"upper", Builtin::Upper, 1, 1},
}};

constexpr bool builtins_sorted() {
    for (size_t index = 1; index < kBuiltins.size(); ++index) {
        if (kBuiltins[index - 1].name >= kBuiltins[index].name) return false;
    }
    return true;
}

static_assert(builtins_sorted(), "the builtin table must stay sorted by name");
static_assert(kBuiltins.size() == static_cast<size_t>(Builtin::GuiSetVisible),
              "every Builtin enumerator except None has a table row");

constexpr bool builtin_ids_unique() {
    for (size_t i = 0; i < kBuiltins.size(); ++i) {
        if (kBuiltins[i].id == Builtin::None) return false;
        for (size_t j = i + 1; j < kBuiltins.size(); ++j) {
            if (kBuiltins[i].id == kBuiltins[j].id) return false;
        }
    }
    return true;
}

static_assert(builtin_ids_unique(), "no Builtin id may be missing or duplicated in the table");

}  // namespace

std::span<const BuiltinInfo> builtin_functions() { return kBuiltins; }

const BuiltinInfo* find_builtin_function(std::string_view name) {
    const auto* found = std::lower_bound(kBuiltins.begin(), kBuiltins.end(), name,
                                         [](const BuiltinInfo& entry, std::string_view needle) {
                                             return entry.name < needle;
                                         });
    if (found == kBuiltins.end() || found->name != name) return nullptr;
    return found;
}

bool is_builtin_function(std::string_view name) { return find_builtin_function(name) != nullptr; }

}  // namespace loomwright
