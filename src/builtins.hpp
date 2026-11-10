#pragma once

#include <string>
#include <vector>

namespace keyloom {

/// One built-in expression function. `max_args` of -1 means "any number of
/// arguments"; otherwise the accepted count is `min_args..max_args`.
struct BuiltinInfo {
    std::string name;
    int min_args = 0;
    int max_args = 0;
};

/// Every built-in function the runtime can evaluate. The checker uses this
/// table to reject unknown names and wrong argument counts before a script
/// runs, and the language guide is checked against it so the two cannot drift
/// apart silently.
const std::vector<BuiltinInfo>& builtin_functions();

const BuiltinInfo* find_builtin_function(const std::string& name);
bool is_builtin_function(const std::string& name);

}  // namespace keyloom
