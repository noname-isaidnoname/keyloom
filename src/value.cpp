#include "value.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>

#include "errors.hpp"

namespace loomwright {
namespace {

constexpr std::string_view kWhitespace = " \t\n\r\v\f";

/// The whitespace-trimmed view of a string.
std::string_view trim(std::string_view text) {
    const size_t begin = text.find_first_not_of(kWhitespace);
    if (begin == std::string_view::npos) return {};
    const size_t end = text.find_last_not_of(kWhitespace);
    return text.substr(begin, end - begin + 1);
}

[[noreturn]] void throw_value_error(std::string_view message) {
    throw RuntimeErrorLoomwright(std::string(message));
}

/// The ASCII-lowercased copy of a string, for case-insensitive spellings.
std::string lower_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    }
    return out;
}

/// The source form of a list: elements in brackets, separated by commas.
std::string format_list(const std::vector<Value>& values) {
    std::string out = "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index > 0) out += ", ";
        out += values[index].repr();
    }
    out += "]";
    return out;
}

}  // namespace

ListBox::ListBox(std::vector<Value> values) {
    storage_ = new Storage{std::move(values), 1};
}

ListBox::ListBox(const ListBox& other) : storage_(other.storage_) {
    if (storage_ != nullptr) ++storage_->references;
}

ListBox& ListBox::operator=(const ListBox& other) {
    if (this == &other) return *this;
    Storage* incoming = other.storage_;
    if (incoming != nullptr) ++incoming->references;
    if (storage_ != nullptr && --storage_->references == 0) delete storage_;
    storage_ = incoming;
    return *this;
}

ListBox::ListBox(ListBox&& other) noexcept : storage_(other.storage_) {
    other.storage_ = nullptr;
}

ListBox& ListBox::operator=(ListBox&& other) noexcept {
    if (this == &other) return *this;
    if (storage_ != nullptr && --storage_->references == 0) delete storage_;
    storage_ = other.storage_;
    other.storage_ = nullptr;
    return *this;
}

ListBox::~ListBox() {
    if (storage_ != nullptr && --storage_->references == 0) delete storage_;
}

std::vector<Value>& ListBox::values() { return storage_->values; }

const std::vector<Value>& ListBox::values() const { return storage_->values; }

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

Value Value::of_list(std::vector<Value> values) {
    Value value;
    value.type = Type::List;
    value.items = ListBox(std::move(values));
    return value;
}

std::vector<Value>& Value::list_items() {
    if (type != Type::List || items.empty()) {
        throw RuntimeErrorLoomwright("expected a list");
    }
    return items.values();
}

const std::vector<Value>& Value::list_items() const {
    if (type != Type::List || items.empty()) {
        throw RuntimeErrorLoomwright("expected a list");
    }
    return items.values();
}

bool Value::truthy() const {
    switch (type) {
        case Type::None: return false;
        case Type::Int: return integer != 0;
        case Type::Float: return real != 0.0;
        case Type::Bool: return boolean;
        case Type::Str: return !text.empty();
        case Type::List: return !items.empty();
    }
    return false;
}

long long Value::as_int() const {
    switch (type) {
        case Type::None: return 0;
        case Type::Int: return integer;
        case Type::Bool: return boolean ? 1 : 0;
        case Type::Float: {
            if (std::isnan(real)) throw_value_error("cannot convert NaN to an integer");
            if (std::isinf(real)) throw_value_error("cannot convert infinity to an integer");
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
            const std::string_view trimmed = trim(text);
            long long parsed = 0;
            const char* begin = trimmed.data();
            const char* end = begin + trimmed.size();
            const auto result = std::from_chars(begin, end, parsed);
            if (trimmed.empty() || result.ec != std::errc() || result.ptr != end) {
                throw_value_error("cannot parse an integer from " + quote_string(text));
            }
            return parsed;
        }
        case Type::List:
            throw_value_error("cannot convert a list to an integer");
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
            const std::string_view trimmed = trim(text);
            if (trimmed.empty()) {
                throw_value_error("cannot convert " + quote_string(text) + " to a number");
            }
            // A leading '+' is not part of the from_chars grammar.
            const std::string_view signed_text = trimmed.front() == '+' ? trimmed.substr(1) : trimmed;
            double parsed = 0.0;
            const char* begin = signed_text.data();
            const char* end = begin + signed_text.size();
            const auto result = std::from_chars(begin, end, parsed);
            if (result.ec == std::errc() && result.ptr == end) return parsed;

            // from_chars rejects the infinity and NaN spellings; accept them.
            const std::string lowered = lower_ascii(signed_text);
            const bool negative = !lowered.empty() && lowered.front() == '-';
            const std::string magnitude = negative ? lowered.substr(1) : lowered;
            if (magnitude == "inf" || magnitude == "infinity") {
                return negative ? -std::numeric_limits<double>::infinity()
                                : std::numeric_limits<double>::infinity();
            }
            if (magnitude == "nan") return std::numeric_limits<double>::quiet_NaN();
            throw_value_error("cannot convert " + quote_string(text) + " to a number");
        }
        case Type::List:
            throw_value_error("cannot convert a list to a number");
    }
    return 0.0;
}

std::string Value::to_string() const {
    switch (type) {
        case Type::None: return "none";
        case Type::Int: return std::to_string(integer);
        case Type::Float: return format_float(real);
        case Type::Bool: return boolean ? "true" : "false";
        case Type::Str: return text;
        case Type::List: return items.empty() ? "[]" : format_list(items.values());
    }
    return "";
}

std::string Value::repr() const {
    switch (type) {
        case Type::None: return "none";
        case Type::Int: return std::to_string(integer);
        case Type::Float: return format_float(real);
        case Type::Bool: return boolean ? "true" : "false";
        case Type::Str: return quote_string(text);
        case Type::List: return items.empty() ? "[]" : format_list(items.values());
    }
    return "";
}

bool value_equals(const Value& left, const Value& right) {
    if (left.type == Value::Type::List || right.type == Value::Type::List) {
        if (left.type != Value::Type::List || right.type != Value::Type::List) return false;
        const std::vector<Value>& left_items = left.list_items();
        const std::vector<Value>& right_items = right.list_items();
        if (left_items.size() != right_items.size()) return false;
        for (size_t index = 0; index < left_items.size(); ++index) {
            if (!value_equals(left_items[index], right_items[index])) return false;
        }
        return true;
    }
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

std::string format_float(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";

    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    std::string out(buffer, result.ptr);
    // Keep the float visible: a whole number still reads as a float.
    if (out.find_first_of(".eE") == std::string::npos) out += ".0";
    return out;
}

std::string quote_string(std::string_view text) {
    static constexpr char kHexDigits[] = "0123456789abcdef";

    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(kHexDigits[(c >> 4) & 0x0F]);
                    out.push_back(kHexDigits[c & 0x0F]);
                } else {
                    out.push_back(raw);
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace loomwright
