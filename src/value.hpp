#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loomwright {

/// A .loom runtime value. The language has four scalar shapes (int, float,
/// bool, string) plus "no value", because script semantics such as
/// `str(1.5)`, `bool("")` and `round()` depend on them. Lists are the
/// container a script uses to collect the strings and numbers it works with.
struct Value {
    enum class Type { None, Int, Float, Bool, Str, List };

    Type type = Type::None;
    long long integer = 0;
    double real = 0.0;
    bool boolean = false;
    std::string text;
    /// List elements. The storage is shared between copies, so `append`
    /// mutates a list in place and every copy sees the change.
    std::shared_ptr<std::vector<Value>> items;

    Value() = default;

    static Value none() { return Value(); }
    static Value of_int(long long v);
    static Value of_float(double v);
    static Value of_bool(bool v);
    static Value of_str(std::string v);
    static Value of_list(std::vector<Value> values);

    bool is_none() const { return type == Type::None; }
    bool is_number() const { return type == Type::Int || type == Type::Float; }
    bool is_list() const { return type == Type::List; }
    bool truthy() const;

    /// The list elements. Throws when the value is not a list.
    std::vector<Value>& list_items();
    const std::vector<Value>& list_items() const;

    /// Integer/float coercion; throws RuntimeErrorLoomwright when the value
    /// cannot be converted.
    long long as_int() const;
    double as_double() const;

    /// The plain text of a value: `none`, `true`, `false`, `1`, `1.0`, the
    /// string itself, or a bracketed list of the elements' plain text.
    std::string to_string() const;
    /// The text of a value in source form: like `to_string()`, except that a
    /// string is quoted and escaped and list elements use their source form.
    std::string repr() const;
};

/// Equality for the scalar shapes the language exposes.
bool value_equals(const Value& left, const Value& right);

/// Shortest round-trip float text ("1.5", "1.0", "1e+16").
std::string format_float(double value);

/// A quoted and escaped string literal ("a\"b\n").
std::string quote_string(std::string_view text);

}  // namespace loomwright
