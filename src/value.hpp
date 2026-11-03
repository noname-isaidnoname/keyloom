#pragma once

#include <string>

namespace keyloom {

/// A .loom runtime value. The language keeps Python's four scalar shapes
/// (int, float, bool, string) plus "no value", because script semantics such as
/// `str(1.5)`, `bool("")` and `round()` depend on them.
struct Value {
    enum class Type { None, Int, Float, Bool, Str };

    Type type = Type::None;
    long long integer = 0;
    double real = 0.0;
    bool boolean = false;
    std::string text;

    Value() = default;

    static Value none() { return Value(); }
    static Value of_int(long long v);
    static Value of_float(double v);
    static Value of_bool(bool v);
    static Value of_str(std::string v);

    bool is_none() const { return type == Type::None; }
    bool is_number() const { return type == Type::Int || type == Type::Float; }
    bool truthy() const;

    /// Python `int(value)`/float coercion; throws RuntimeErrorKeyloom when the
    /// value cannot be converted.
    long long as_int() const;
    double as_double() const;

    /// Python `str(value)`.
    std::string to_string() const;
    /// Python `repr(value)`.
    std::string repr() const;
};

/// Python's `==` for the scalar shapes the language exposes.
bool value_equals(const Value& left, const Value& right);

/// Shortest round-trip float text in Python's `repr` style ("1.0", "1e+16").
std::string py_float_str(double value);

/// Python's `repr()` of a string, including quote selection and escapes.
std::string py_repr_string(const std::string& text);

}  // namespace keyloom
