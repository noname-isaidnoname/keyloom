#pragma once

#include <string>
#include <vector>

#include "errors.hpp"
#include "value.hpp"

namespace loomwright {

enum class TokenKind { Space, Comment, String, Duration, Number, Ident, Op, Newline, Eof };

const char* token_kind_name(TokenKind kind);

struct Token {
    TokenKind kind = TokenKind::Eof;
    Value value;
    std::string raw;
    int line = 1;
    int col = 1;
};

/// Split .loom source into tokens. Mirrors the original scanner exactly:
/// whitespace and `#` comments vanish, `\n` becomes a NEWLINE token, and the
/// decoded string value is produced in a single pass.
std::vector<Token> tokenize(const std::string& source);

/// Decode the language's escape set once (`\\n` stays a backslash plus `n`).
std::string decode_string(const std::string& body);

}  // namespace loomwright
