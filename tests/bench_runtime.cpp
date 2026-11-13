// A small, self-contained benchmark for the Loomwright front end and runtime.
//
// It measures the parts a script spends its time in: parsing a script, the
// arithmetic hot path, and a call-heavy workload that forces each expression
// through the awaiting evaluator (one coroutine frame per user function
// call). The first workload used to print the result once per repetition,
// which made the timing depend on stdout; both workloads are print-free, so
// the numbers measure the language, not the console.
//
//   loomwright_bench [repetitions] [trials]
//
// Each workload is measured in `trials` independent passes of `repetitions`
// reps after a warm-up pass, and both the best and the median time per
// repetition are reported. The best time tracks the code generation (the
// noise floor of the machine); the median tracks a typical run. Compare both
// before and after a change, same build settings. For the least noise run it
// pinned to one core, e.g. `taskset -c 0 ./loomwright_bench 20 7`.
//
// The workloads are deliberately synthetic, which is what a script does
// between two key presses.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "backends.hpp"
#include "parser.hpp"
#include "runtime.hpp"
#include "value.hpp"

namespace {
std::size_t g_allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
    ++g_allocations;
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) throw std::bad_alloc();
    return memory;
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

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

const char* const kArithScript = R"(loom "bench" {
    when app starts {
        set total = 0
        set label = ""
        for i = 1 to 10000 {
            for j = 1 to 10 {
                set total = total + i * j
            }
            set label = label + "x"
        }
    }
})";

// Every call goes through the awaiting evaluator: the expression may wait, so
// each `double(i)` allocates an evaluator frame and a callee frame and hands
// the awaiting frame back to the scheduler. This is the workload that sees
// the scheduler's deferred-resume path.
const char* const kAwaitScript = R"(loom "bench-await" {
    func double(x) {
        return x * 2
    }
    when app starts {
        set total = 0
        for i = 1 to 10000 {
            set total = (total + double(i)) % 1000003
        }
    }
})";

struct Workload {
    const char* name;
    const char* script;
    const char* checksum_variable;
};

double seconds_since(const std::chrono::steady_clock::time_point& start) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double>(elapsed).count();
}

/// Run `repetitions` reps of `workload` and return the seconds and the
/// allocation count for the whole batch. The event and the runtime are
/// reused across reps, exactly as the runtime is reused across a session.
std::pair<double, std::size_t> run_batch(Runtime& runtime, const loomwright::Node& event, int repetitions) {
    const std::size_t allocations_before = g_allocations;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < repetitions; ++i) {
        runtime.scheduler().spawn(runtime.run_event(event));
        runtime.scheduler().run();
    }
    const double seconds = seconds_since(start);
    return {seconds, g_allocations - allocations_before};
}

}  // namespace

int main(int argc, char** argv) {
    const int repetitions = argc > 1 ? std::atoi(argv[1]) : 20;
    const int trials = argc > 2 ? std::atoi(argv[2]) : 7;
    if (repetitions <= 0) {
        std::fprintf(stderr, "repetitions must be positive\n");
        return 2;
    }
    if (trials <= 0) {
        std::fprintf(stderr, "trials must be positive\n");
        return 2;
    }

    // 1. Front end: tokenize, parse and check the same script repeatedly.
    const auto parse_start = std::chrono::steady_clock::now();
    std::vector<double> parse_times;
    size_t statements = 0;
    for (int trial = 0; trial < trials; ++trial) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < repetitions; ++i) {
            const Script script = loomwright::parse(kArithScript, "bench.loom");
            statements += script.events.size();
        }
        parse_times.push_back(seconds_since(start) / repetitions * 1000.0);
    }
    std::sort(parse_times.begin(), parse_times.end());
    const auto parse_seconds = seconds_since(parse_start);

    const Workload workloads[] = {
        {"arith", kArithScript, "total"},
        {"await", kAwaitScript, "total"},
    };

    std::printf("compiler      : %s\n", LOOMWRIGHT_BENCH_COMPILER);
    std::printf("repetitions   : %d\n", repetitions);
    std::printf("trials        : %d\n", trials);
    std::printf("parse         : %.2f ms per script (median %.4f ms)\n",
                parse_seconds / (static_cast<double>(trials) * repetitions) * 1000.0,
                parse_times[parse_times.size() / 2]);

    for (const Workload& workload : workloads) {
        Script script = loomwright::parse(workload.script, "bench.loom");
        Runtime runtime(script, Runtime::Options{});
        runtime.set_backend(std::make_unique<NullBackend>());

        // Warm up once so the pages and the branch predictors are hot before
        // the timed passes.
        run_batch(runtime, script.events.at(0), 1);

        std::vector<double> times;
        times.reserve(static_cast<size_t>(trials));
        std::size_t allocations = 0;
        for (int trial = 0; trial < trials; ++trial) {
            const auto [seconds, count] = run_batch(runtime, script.events.at(0), repetitions);
            times.push_back(seconds / repetitions * 1000.0);
            allocations = count;  // allocations are deterministic; keep the last one
        }
        std::sort(times.begin(), times.end());
        const double median = times[times.size() / 2];

        std::printf("%-13s : best %.2f ms  median %.2f ms  worst %.2f ms  allocations %zu (%.1f per run)\n",
                    workload.name, times.front(), median, times.back(), allocations,
                    allocations / static_cast<double>(repetitions));
        const Value checksum = runtime.variables().at(workload.checksum_variable);
        std::printf("%-13s   checksum %s\n", "", checksum.to_string().c_str());
    }
    std::printf("events parsed : %zu\n", statements);
    return 0;
}
