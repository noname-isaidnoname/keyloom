#pragma once

#include <string>
#include <vector>

namespace loomwright {

struct CommandResult {
    int returncode = -1;
    std::string out;
    std::string err;
    bool timed_out = false;
    bool spawn_failed = false;
    std::string spawn_error;
};

/// Resolve an executable through PATH (or accept an explicit path).
std::string which(const std::string& name);

/// Run a command without a shell, optionally feeding stdin and bounding the
/// runtime. Never throws for a non-zero exit status.
CommandResult run_command(const std::vector<std::string>& argv, const std::string& input = std::string(),
                          int timeout_ms = 0);

/// Start a program in its own session with stdio redirected, without waiting.
bool spawn_detached(const std::vector<std::string>& argv, std::string& error);

/// POSIX-shell-style word splitting: single quotes, double quotes, backslash
/// escapes and `#` comments, without any shell being involved.
std::vector<std::string> split_shell_words(const std::string& text);

}  // namespace loomwright
