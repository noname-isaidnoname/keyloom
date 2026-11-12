#include "harness.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>

namespace loomwright_test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void fail(const std::string& location, const std::string& message) {
    throw std::runtime_error(location + ": " + message);
}

int run_all(const std::string& filter) {
    int failures = 0;
    size_t executed = 0;
    for (const auto& test : registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) continue;
        ++executed;
        try {
            test.function();
            std::cout << "ok   " << test.name << "\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << test.name << "\n     " << error.what() << "\n";
        } catch (...) {
            ++failures;
            std::cout << "FAIL " << test.name << "\n     unknown exception\n";
        }
    }
    std::cout << (executed - static_cast<size_t>(failures)) << "/" << executed << " tests passed\n";
    return failures == 0 ? 0 : 1;
}

}  // namespace loomwright_test

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : std::string();
    return loomwright_test::run_all(filter);
}
