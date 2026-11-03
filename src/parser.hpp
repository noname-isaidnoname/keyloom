#pragma once

#include <stdexcept>
#include <string>

#include "ast.hpp"

namespace keyloom {

/// A syntax error. `line`/`col` are 0 when the error has no token attached.
class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& message, int line_number = 0, int column = 0)
        : std::runtime_error(message), line(line_number), col(column) {}

    int line = 0;
    int col = 0;
};

Script parse(const std::string& source, const std::string& path = "<memory>");

}  // namespace keyloom
