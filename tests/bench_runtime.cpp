// A small, self-contained benchmark for the Loomwright front end and runtime.
//
// It measures the two parts a script spends its time in: parsing a script and
// evaluating it. The numbers are only useful when they are compared with the
// same build settings, so the report prints the compiler and the build type.
//
//   loomwright_bench [repetitions]
//
// The workload is deliberately synthetic: a loop of integer arithmetic and a
// string concatenation, which is what a script does between two key presses.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "backends.hpp"
#include "parser.hpp"
#include "runtime.hpp"
#include "value.hpp"

namespace {

using loomwright::Runtime;
using loomwright::Script;
using loomwright::Value;

/// A backend that accepts every action and records nothing.
class NullBackend : public loomwright::OutputBackend {
public:
    void warp(int, int) override {}
    void move(int, int) override {}
    void button(const std::string&, bool) override {}
    void key(const std::string&, bool) override {}
    void type_text(const std::string&) override {}
    void scroll(int, int) override {}
    void close() override {}
    const char* name() const override { return "NullBackend"; }
};

const char* const kScript = R"(loom "bench" {
    when app starts {
        set total = 0
        set label = ""
        for i = 1 to 10000 {
            for j = 1 to 10 {
                set total = total + i * j
            }
            set label = label + "x"
        }
        log str(total)
    }
})";

double seconds_since(const std::chrono::steady_clock::time_point& start) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double>(elapsed).count();
}

}  // namespace

int main(int argc, char** argv) {
    const int repetitions = argc > 1 ? std::atoi(argv[1]) : 20;
    if (repetitions <= 0) {
        std::fprintf(stderr, "repetitions must be positive\n");
        return 2;
    }

    // 1. Front end: tokenize, parse and check the same script repeatedly.
    const auto parse_start = std::chrono::steady_clock::now();
    size_t statements = 0;
    for (int i = 0; i < repetitions; ++i) {
        const Script script = loomwright::parse(kScript, "bench.loom");
        statements += script.events.size();
    }
    const double parse_seconds = seconds_since(parse_start);

    // 2. Runtime: run the event handler repeatedly.
    Script script = loomwright::parse(kScript, "bench.loom");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<NullBackend>());
    const auto run_start = std::chrono::steady_clock::now();
    for (int i = 0; i < repetitions; ++i) {
        runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
        runtime.scheduler().run();
    }
    const double run_seconds = seconds_since(run_start);
    const Value total = runtime.variables().at("total");

    std::printf("compiler      : %s\n", LOOMWRIGHT_BENCH_COMPILER);
    std::printf("repetitions   : %d\n", repetitions);
    std::printf("parse         : %.4f s (%.2f ms per script)\n", parse_seconds,
                parse_seconds * 1000.0 / repetitions);
    std::printf("run           : %.4f s (%.2f ms per run)\n", run_seconds,
                run_seconds * 1000.0 / repetitions);
    std::printf("checksum      : %s\n", total.to_string().c_str());
    std::printf("events parsed : %zu\n", statements);
    return 0;
}
