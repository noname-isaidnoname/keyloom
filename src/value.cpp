#include "value.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "errors.hpp"

namespace keyloom {
namespace {

const char* kInvalidInt = "invalid literal for int()";
const char* kInvalidFloat = "could not convert string to float";

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

std::string strip(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

[[noreturn]] void throw_value_error(const std::string& message) {
    throw RuntimeErrorKeyloom(message);
}

}  // namespace

Value Value::of_int(long long v) {
    Value value;
    value.type = Type::Int;
    value.integer = v;
    return value;
}

Value Value::of_float(double v) {
    Value value;
    value.type = Type::Float;
    value.real = v;
    return value;
}

Value Value::of_bool(bool v) {
    Value value;
    value.type = Type::Bool;
    value.boolean = v;
    return value;
}

Value Value::of_str(std::string v) {
    Value value;
    value.type = Type::Str;
    value.text = std::move(v);
    return value;
}

bool Value::truthy() const {
    switch (type) {
        case Type::None: return false;
        case Type::Int: return integer != 0;
        case Type::Float: return real != 0.0;
        case Type::Bool: return boolean;
        case Type::Str: return !text.empty();
    }
    return false;
}

long long Value::as_int() const {
    switch (type) {
        case Type::None: return 0;
        case Type::Int: return integer;
        case Type::Bool: return boolean ? 1 : 0;
        case Type::Float: {
            if (std::isnan(real)) throw_value_error(std::string(kInvalidInt) + ": cannot convert float NaN to integer");
            if (std::isinf(real)) throw_value_error(std::string(kInvalidInt) + ": cannot convert float infinity to integer");
            const double truncated = std::trunc(real);
            if (truncated >= static_cast<double>(std::numeric_limits<long long>::max())) {
                return std::numeric_limits<long long>::max();
            }
            if (truncated <= static_cast<double>(std::numeric_limits<long long>::min())) {
                return std::numeric_limits<long long>::min();
            }
            return static_cast<long long>(truncated);
        }
        case Type::Str: {
            const std::string trimmed = strip(text);
            long long parsed = 0;
            const char* begin = trimmed.data();
            const char* end = begin + trimmed.size();
            auto result = std::from_chars(begin, end, parsed);
            if (trimmed.empty() || result.ec != std::errc() || result.ptr != end) {
                throw_value_error(std::string(kInvalidInt) + " with base 10: " + py_repr_string(text));
            }
            return parsed;
        }
    }
    return 0;
}

double Value::as_double() const {
    switch (type) {
        case Type::None: return 0.0;
        case Type::Int: return static_cast<double>(integer);
        case Type::Bool: return boolean ? 1.0 : 0.0;
        case Type::Float: return real;
        case Type::Str: {
            const std::string trimmed = strip(text);
            if (trimmed.empty()) {
                throw_value_error(std::string(kInvalidFloat) + ": " + py_repr_string(text));
            }
            errno = 0;
            char* parsed_end = nullptr;
            const double parsed = std::strtod(trimmed.c_str(), &parsed_end);
            if (parsed_end != trimmed.c_str() + trimmed.size() || errno == ERANGE) {
                throw_value_error(std::string(kInvalidFloat) + ": " + py_repr_string(text));
            }
            return parsed;
        }
    }
    return 0.0;
}

std::string Value::to_string() const {
    switch (type) {
        case Type::None: return "None";
        case Type::Int: return std::to_string(integer);
        case Type::Float: return py_float_str(real);
        case Type::Bool: return boolean ? "True" : "False";
        case Type::Str: return text;
    }
    return "";
}

std::string Value::repr() const {
    switch (type) {
        case Type::None: return "None";
        case Type::Int: return std::to_string(integer);
        case Type::Float: return py_float_str(real);
        case Type::Bool: return boolean ? "True" : "False";
        case Type::Str: return py_repr_string(text);
    }
    return "";
}

bool value_equals(const Value& left, const Value& right) {
    const bool left_numeric = left.is_number() || left.type == Value::Type::Bool;
    const bool right_numeric = right.is_number() || right.type == Value::Type::Bool;
    if (left_numeric && right_numeric) {
        // Integer pairs compare exactly so large values stay precise.
        if (left.type != Value::Type::Float && right.type != Value::Type::Float) {
            return left.as_int() == right.as_int();
        }
        return left.as_double() == right.as_double();
    }
    if (left.type == Value::Type::Str && right.type == Value::Type::Str) return left.text == right.text;
    if (left.type == Value::Type::None || right.type == Value::Type::None) {
        return left.type == right.type;
    }
    return false;
}

std::string py_float_str(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";

    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
    std::string scientific(buffer, result.ptr);
    const size_t exponent_position = scientific.find('e');
    if (exponent_position == std::string::npos) return scientific;

    std::string mantissa = scientific.substr(0, exponent_position);
    const int exponent = std::stoi(scientific.substr(exponent_position + 1));

    bool negative = false;
    if (!mantissa.empty() && mantissa.front() == '-') {
        negative = true;
        mantissa.erase(mantissa.begin());
    }
    std::string digits;
    for (char c : mantissa) {
        if (c != '.') digits.push_back(c);
    }

    // Python prints fixed notation while -4 < decimal_point <= 16, and
    // scientific notation otherwise.
    const int decimal_point = exponent + 1;
    std::string out;
    if (decimal_point > -4 && decimal_point <= 16) {
        if (decimal_point <= 0) {
            out = "0." + std::string(static_cast<size_t>(-decimal_point), '0') + digits;
        } else if (static_cast<size_t>(decimal_point) >= digits.size()) {
            out = digits + std::string(static_cast<size_t>(decimal_point) - digits.size(), '0') + ".0";
        } else {
            out = digits.substr(0, static_cast<size_t>(decimal_point)) + "." +
                  digits.substr(static_cast<size_t>(decimal_point));
        }
    } else {
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        out += "e";
        out += exponent < 0 ? "-" : "+";
        const int absolute = exponent < 0 ? -exponent : exponent;
        if (absolute < 10) out += "0";
        out += std::to_string(absolute);
    }
    return negative ? "-" + out : out;
}

std::string py_repr_string(const std::string& text) {
    const bool has_single = text.find('\'') != std::string::npos;
    const bool has_double = text.find('"') != std::string::npos;
    const char quote = (has_single && !has_double) ? '"' : '\'';

    std::string out;
    out.push_back(quote);
    for (unsigned char c : text) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == static_cast<unsigned char>(quote)) {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20 || c == 0x7f) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\x%02x", c);
            out += buffer;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    out.push_back(quote);
    return out;
}

}  // namespace keyloom
