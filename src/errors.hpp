#pragma once

#include <stdexcept>
#include <string>

namespace loomwright {

/// A backend (output device, key table, tool process) could not do its job.
class BackendError : public std::runtime_error {
public:
    explicit BackendError(const std::string& message) : std::runtime_error(message) {}
};

/// A script failed at runtime; printed as `loomwright: ...` with exit status 1.
class RuntimeErrorLoomwright : public std::runtime_error {
public:
    explicit RuntimeErrorLoomwright(const std::string& message) : std::runtime_error(message) {}
};

/// The lexer rejected a character.
class LexError : public std::runtime_error {
public:
    explicit LexError(const std::string& message) : std::runtime_error(message) {}
};

/// Thrown into a coroutine that was cancelled at an await point.
struct TaskCancelled {};

}  // namespace loomwright
