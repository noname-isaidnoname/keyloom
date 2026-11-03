#include "harness.hpp"

#include "scheduler.hpp"

using namespace keyloom;

namespace {

int g_stage = 0;

// Mirrors Runtime::run_body, which is a *child* of the spawned Task and only
// registers in known_, never in live_. request_stop() must resume it anyway —
// gating waiter resumption on live_ deadlocked Ctrl+C.
Task awaited_child() {
    co_await wait_stop();
    g_stage = 1;
}

Task root_task() {
    co_await awaited_child();
    g_stage = 2;
}

Task stopper() {
    co_await sleep_for(0.02);
    Scheduler::current().request_stop();
}

}  // namespace

TEST_CASE(request_stop_wakes_an_awaited_child) {
    g_stage = 0;
    Scheduler scheduler;
    scheduler.spawn(root_task());
    scheduler.spawn(stopper());
    scheduler.run();
    CHECK_EQ(g_stage, 2);
}
