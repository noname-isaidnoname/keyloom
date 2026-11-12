#pragma once

#include <string>
#include <vector>

#include "ast.hpp"

namespace loomwright {

struct Diagnostic {
    std::string level;  // "error" | "warning" | "ok"
    std::string message;
    int line = 0;  // 0 means "no line"

    std::string to_string() const;
};

/// Static checks: option values, key names, timer intervals, hold-token balance
/// and /dev/uinput availability. Never opens input devices.
std::vector<Diagnostic> check_script(const Script& script);

}  // namespace loomwright
