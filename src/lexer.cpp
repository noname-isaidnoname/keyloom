#include "lexer.hpp"

#include <cctype>
#include <cstddef>

namespace loomwright {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool is_ident_body(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool is_word(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

size_t utf8_char_count(const std::string& text) {
    size_t chars = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++chars;
    }
    return chars;
}

/// The first line of the source at `position`, at most 24 characters long.
std::string_view first_line_snippet(std::string_view source, size_t position) {
    const std::string_view slice = source.substr(position, 24);
    const size_t newline = slice.find('\n');
    return newline == std::string_view::npos ? slice : slice.substr(0, newline);
}

[[noreturn]] void throw_unexpected(std::string_view source, size_t position, size_t line, size_t col) {
    const std::string character(1, source[position]);
    std::string message = "Unexpected character " + quote_string(character);
    message += " at " + std::to_string(line) + ":" + std::to_string(col);
    message += " near " + quote_string(first_line_snippet(source, position));
    throw LexError(message);
}

bool has_duration_unit(const std::string& source, size_t position, size_t& length) {
    if (position >= source.size()) return false;
    if (source.compare(position, 2, "ms") == 0) {
        length = 2;
        return position + 2 >= source.size() || !is_word(source[position + 2]);
    }
    const char unit = source[position];
    if (unit == 's' || unit == 'm' || unit == 'h') {
        length = 1;
        return position + 1 >= source.size() || !is_word(source[position + 1]);
    }
    return false;
}

double duration_factor(const std::string& unit) {
    if (unit == "ms") return 0.001;
    if (unit == "s") return 1.0;
    if (unit == "m") return 60.0;
    return 3600.0;
}

}  // namespace

const char* token_kind_name(TokenKind kind) {
    switch (kind) {
        case TokenKind::Space: return "SPACE";
        case TokenKind::Comment: return "COMMENT";
        case TokenKind::String: return "STRING";
        case TokenKind::Duration: return "DURATION";
        case TokenKind::Number: return "NUMBER";
        case TokenKind::Ident: return "IDENT";
        case TokenKind::Op: return "OP";
        case TokenKind::Newline: return "NEWLINE";
        case TokenKind::Eof: return "EOF";
    }
    return "EOF";
}

std::string decode_string(std::string_view body) {
    std::string out;
    out.reserve(body.size());
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\' || i + 1 >= body.size()) {
            out.push_back(body[i]);
            continue;
        }
        const char escaped = body[i + 1];
        switch (escaped) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '\\': out.push_back('\\'); break;
            case '\'': out.push_back('\''); break;
            case '"': out.push_back('"'); break;
            default:
                out.push_back('\\');
                out.push_back(escaped);
                break;
        }
        ++i;
    }
    return out;
}

std::vector<Token> tokenize(const std::string& source) {
    std::vector<Token> out;
    // A source line usually holds a handful of tokens; reserving keeps the
    // vector from growing while it is filled.
    out.reserve(source.size() / 8 + 8);
    size_t position = 0;
    size_t line = 1;
    size_t col = 1;

    while (position < source.size()) {
        const char c = source[position];

        if (is_space(c)) {
            size_t length = 0;
            while (position + length < source.size() && is_space(source[position + length])) ++length;
            position += length;
            col += length;
            continue;
        }
        if (c == '#') {
            while (position < source.size() && source[position] != '\n') ++position;
            continue;
        }
        if (c == '\n') {
            Token token;
            token.kind = TokenKind::Newline;
            token.value = Value::none();
            token.line = static_cast<int>(line);
            token.col = static_cast<int>(col);
            out.push_back(std::move(token));
            ++position;
            ++line;
            col = 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char quote = c;
            size_t scan = position + 1;
            bool closed = false;
            while (scan < source.size()) {
                if (source[scan] == '\\' && scan + 1 < source.size()) {
                    scan += 2;
                    continue;
                }
                if (source[scan] == quote) {
                    closed = true;
                    break;
                }
                if (source[scan] == '\n') break;
                ++scan;
            }
            if (!closed) throw_unexpected(source, position, line, col);
            const std::string body = source.substr(position + 1, scan - position - 1);
            const std::string raw = source.substr(position, scan - position + 1);
            Token token;
            token.kind = TokenKind::String;
            token.value = Value::of_str(decode_string(body));
            token.line = static_cast<int>(line);
            token.col = static_cast<int>(col);
            out.push_back(std::move(token));
            const size_t consumed = raw.size();
            position += consumed;
            col += utf8_char_count(raw);
            continue;
        }
        if (is_digit(c)) {
            size_t scan = position;
            while (scan < source.size() && is_digit(source[scan])) ++scan;
            const size_t integer_end = scan;
            bool is_float = false;
            if (scan < source.size() && source[scan] == '.' && scan + 1 < source.size() &&
                is_digit(source[scan + 1])) {
                is_float = true;
                scan += 1;
                while (scan < source.size() && is_digit(source[scan])) ++scan;
            }
            size_t unit_length = 0;
            const bool duration = has_duration_unit(source, scan, unit_length);
            const std::string raw = source.substr(position, scan - position + unit_length);
            Token token;
            token.line = static_cast<int>(line);
            token.col = static_cast<int>(col);
            if (duration) {
                const std::string number_text = source.substr(position, scan - position);
                const std::string unit = source.substr(scan, unit_length);
                token.kind = TokenKind::Duration;
                token.value = Value::of_float(std::stod(number_text) * duration_factor(unit));
            } else if (is_float) {
                token.kind = TokenKind::Number;
                token.value = Value::of_float(std::stod(raw));
            } else {
                token.kind = TokenKind::Number;
                token.value = Value::of_int(std::stoll(source.substr(position, integer_end - position)));
            }
            out.push_back(std::move(token));
            position += raw.size();
            col += raw.size();
            continue;
        }
        if (is_ident_start(c)) {
            size_t scan = position;
            while (scan < source.size() && is_ident_body(source[scan])) ++scan;
            const std::string raw = source.substr(position, scan - position);
            std::string lowered;
            lowered.reserve(raw.size());
            for (char ch : raw) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            Token token;
            token.line = static_cast<int>(line);
            token.col = static_cast<int>(col);
            if (lowered == "and" || lowered == "or" || lowered == "not") {
                token.kind = TokenKind::Op;
                token.value = Value::of_str(lowered);
            } else {
                token.kind = TokenKind::Ident;
                token.value = Value::of_str(raw);
            }
            out.push_back(std::move(token));
            position += raw.size();
            col += raw.size();
            continue;
        }

        static const char* const kDoubleOperators[] = {"==", "!=", "<=", ">=", "&&", "||"};
        bool matched = false;
        for (const char* op : kDoubleOperators) {
            if (source.compare(position, 2, op) == 0) {
                Token token;
                token.kind = TokenKind::Op;
                token.value = Value::of_str(op);
                    token.line = static_cast<int>(line);
                token.col = static_cast<int>(col);
                out.push_back(std::move(token));
                position += 2;
                col += 2;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        static const std::string kSingleOperators = "{}();,.=+*/%<>!-[]";
        if (kSingleOperators.find(c) != std::string::npos) {
            Token token;
            token.kind = TokenKind::Op;
            token.value = Value::of_str(std::string(1, c));
            token.line = static_cast<int>(line);
            token.col = static_cast<int>(col);
            out.push_back(std::move(token));
            ++position;
            ++col;
            continue;
        }

        throw_unexpected(source, position, line, col);
    }

    Token eof;
    eof.kind = TokenKind::Eof;
    eof.value = Value::none();
    eof.line = static_cast<int>(line);
    eof.col = static_cast<int>(col);
    out.push_back(std::move(eof));
    return out;
}

}  // namespace loomwright
