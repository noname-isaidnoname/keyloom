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

namespace {

int g_cancel_stage = 0;

Task cancel_leaf() {
    co_await sleep_for(0.05);
    g_cancel_stage = 3;
}

Task cancel_middle() {
    co_await cancel_leaf();
    g_cancel_stage = 2;
}

Task cancel_root() {
    co_await cancel_middle();
    g_cancel_stage = 1;
}

Task cancel_driver(Task::handle_type victim) {
    co_await sleep_for(0.01);
    Scheduler::current().cancel(victim);
    // Stay alive past the leaf's deadline so an orphaned frame would resume.
    co_await sleep_for(0.1);
}

}  // namespace

TEST_CASE(cancelling_a_task_wakes_the_frame_that_is_suspended) {
    g_cancel_stage = 0;
    Scheduler scheduler;
    Task victim = cancel_root();
    const Task::handle_type handle = victim.handle;
    scheduler.spawn(std::move(victim));
    scheduler.spawn(cancel_driver(handle));
    scheduler.run();
    CHECK_EQ(g_cancel_stage, 0);
    CHECK_EQ(scheduler.live_count(), static_cast<size_t>(0));
}
