#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace loomwright {

/// The built-in expression functions the runtime implements. The parser
/// resolves a call's target once, so evaluation dispatches on an enum instead
/// of comparing spellings while a script runs. `None` marks a call that is
/// not a built-in (a user-defined function).
enum class Builtin : std::uint8_t {
    None,
    Min,
    Max,
    Abs,
    Round,
    Floor,
    Ceil,
    Sqrt,
    Clamp,
    Int,
    Float,
    IsNumber,
    Str,
    Bool,
    Len,
    Random,
    Randint,
    Elapsed,
    Upper,
    Lower,
    Trim,
    StartsWith,
    EndsWith,
    Contains,
    Replace,
    Substr,
    IndexOf,
    Join,
    Split,
    Append,
    Sort,
    KeyDown,
    MouseHeld,
    SuspendHotkeys,
    HotkeysSuspended,
    SetHotkeyEnabled,
    HotkeyEnabled,
    ScreenWidth,
    ScreenHeight,
    ActiveWindowTitle,
    ActiveWindowClass,
    ActiveWindowProcess,
    ActiveWindowPid,
    GuiAlert,
    GuiInput,
    GuiConfirm,
    GuiValue,
    GuiSetText,
    GuiSetVisible,
};

/// One built-in expression function. `max_args` of -1 means "any number of
/// arguments"; otherwise the accepted count is `min_args..max_args`. The table
/// is sorted by name so the lookup is a binary search over a compile-time
/// array.
struct BuiltinInfo {
    std::string_view name;
    Builtin id = Builtin::None;
    int min_args = 0;
    int max_args = 0;
};

/// Every built-in function the runtime can evaluate. The checker uses this
/// table to reject unknown names and wrong argument counts before a script
/// runs, and the language guide is checked against it so the two cannot drift
/// apart silently.
std::span<const BuiltinInfo> builtin_functions();

const BuiltinInfo* find_builtin_function(std::string_view name);
bool is_builtin_function(std::string_view name);

}  // namespace loomwright
