#pragma once

#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace keyloom_test {

struct TestCase {
    std::string name;
    void (*function)();
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* name, void (*function)()) { registry().push_back(TestCase{name, function}); }
};

void fail(const std::string& location, const std::string& message);

template <typename T>
std::string to_text(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

inline std::string to_text(bool value) { return value ? "true" : "false"; }

inline std::string to_text(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += ", ";
        out += "\"" + value + "\"";
    }
    return "[" + out + "]";
}

/// stdout/stderr captured while `fn` ran.
struct CapturedOutput {
    int code = 0;
    std::string out;
    std::string err;
};

/// Run `fn` with stdout and stderr redirected to pipes and return what it printed.
inline CapturedOutput capture_output(const std::function<int()>& fn) {
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        throw std::runtime_error("capture_output: pipe failed");
    }
    const int saved_out = dup(STDOUT_FILENO);
    const int saved_err = dup(STDERR_FILENO);
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[1]);
    close(err_pipe[1]);

    CapturedOutput result;
    try {
        result.code = fn();
    } catch (...) {
        dup2(saved_out, STDOUT_FILENO);
        dup2(saved_err, STDERR_FILENO);
        close(saved_out);
        close(saved_err);
        close(out_pipe[0]);
        close(err_pipe[0]);
        throw;
    }
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);

    const auto drain = [](int fd) {
        std::string text;
        char buffer[4096];
        ssize_t count = 0;
        while ((count = read(fd, buffer, sizeof(buffer))) > 0) text.append(buffer, static_cast<size_t>(count));
        close(fd);
        return text;
    };
    result.out = drain(out_pipe[0]);
    result.err = drain(err_pipe[0]);
    return result;
}

/// Trim trailing newlines so CHECK_EQ reads naturally.
inline std::string strip_trailing_newlines(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

int run_all(const std::string& filter = std::string());

}  // namespace keyloom_test

#define TEST_CASE(name)                                              \
    static void name();                                              \
    static ::keyloom_test::Registrar registrar_##name(#name, &name); \
    static void name()

#define CHECK_TRUE(expression)                                              \
    do {                                                                    \
        if (!(expression)) {                                                \
            ::keyloom_test::fail(std::string(__FILE__) + ":" + std::to_string(__LINE__), \
                                 "CHECK_TRUE(" #expression ")");       \
        }                                                                   \
    } while (false)

#define CHECK_EQ(actual, expected)                                                              \
    do {                                                                                        \
        const auto& actual_value_ = (actual);                                                   \
        const auto& expected_value_ = (expected);                                               \
        if (!(actual_value_ == expected_value_)) {                                              \
            ::keyloom_test::fail(std::string(__FILE__) + ":" + std::to_string(__LINE__),       \
                                 std::string("CHECK_EQ(" #actual ", " #expected ")\n  actual:   ") + \
                                     ::keyloom_test::to_text(actual_value_) +                   \
                                     "\n  expected: " + ::keyloom_test::to_text(expected_value_)); \
        }                                                                                       \
    } while (false)

#define CHECK_THROWS(expression, exception_type)                                    \
    do {                                                                           \
        bool caught_ = false;                                                      \
        try {                                                                      \
            (void)(expression);                                                    \
        } catch (const exception_type&) {                                          \
            caught_ = true;                                                        \
        } catch (...) {                                                            \
        }                                                                          \
        if (!caught_) {                                                            \
            ::keyloom_test::fail(std::string(__FILE__) + ":" + std::to_string(__LINE__), \
                                 "expected " #exception_type " from " #expression); \
        }                                                                          \
    } while (false)
