#include "harness.hpp"

#include "scheduler.hpp"

#include <unistd.h>
#include <cerrno>
#include <memory>

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

namespace {

Task cancelled_then_waits(double& elapsed) {
    try {
        co_await sleep_for(0.04);
    } catch (const TaskCancelled&) {
    }
    const double start = Scheduler::current().now();
    try {
        co_await sleep_for(0.12);
    } catch (const TaskCancelled&) {
    }
    elapsed = Scheduler::current().now() - start;
}

Task cancel_once(Task::handle_type victim) {
    co_await sleep_for(0.01);
    Scheduler::current().cancel(victim);
}

}  // namespace

TEST_CASE(cancelled_timer_cannot_wake_a_later_wait_on_the_same_frame) {
    double elapsed = 0.0;
    Scheduler scheduler;
    Task victim = cancelled_then_waits(elapsed);
    scheduler.spawn(victim);
    scheduler.spawn(cancel_once(victim.handle));
    scheduler.run();
    CHECK_TRUE(elapsed >= 0.09);
}

namespace {

Task stopped_then_waits(double& elapsed) {
    try {
        co_await wait_stop();
    } catch (const TaskCancelled&) {
    }
    const double start = Scheduler::current().now();
    try {
        co_await sleep_for(0.1);
    } catch (const TaskCancelled&) {
    }
    elapsed = Scheduler::current().now() - start;
}

Task stop_and_cancel(Task::handle_type victim) {
    co_await sleep_for(0.01);
    Scheduler::current().request_stop();
    Scheduler::current().cancel(victim);
}

Task mark_started(bool& started) {
    started = true;
    co_return;
}

}  // namespace

TEST_CASE(stop_then_cancel_does_not_wake_next_suspension) {
    double elapsed = 0.0;
    Scheduler scheduler;
    Task victim = stopped_then_waits(elapsed);
    scheduler.spawn(victim);
    scheduler.spawn(stop_and_cancel(victim.handle));
    scheduler.run();
    CHECK_TRUE(elapsed >= 0.07);
}

TEST_CASE(cancel_before_start_does_not_enter_task) {
    bool started = false;
    Scheduler scheduler;
    Task victim = mark_started(started);
    scheduler.spawn(victim);
    scheduler.cancel(victim.handle);
    scheduler.run();
    CHECK_TRUE(!started);
    CHECK_EQ(scheduler.live_count(), static_cast<size_t>(0));
}

namespace {

Task wait_on_pipe(int fd, bool& awakened) {
    try {
        co_await readable(fd);
        awakened = true;
    } catch (const TaskCancelled&) {
    }
}

Task duplicate_pipe_wait(int fd, bool& rejected) {
    try {
        co_await readable(fd);
    } catch (const RuntimeErrorKeyloom&) {
        rejected = true;
    }
}

Task signal_pipe(int fd, Task::handle_type first) {
    co_await sleep_for(0.01);
    const char byte = 'x';
    (void)::write(fd, &byte, 1);
    // Avoid hanging the test if the first registration was lost.
    co_await sleep_for(0.05);
    Scheduler::current().cancel(first);
}

}  // namespace

TEST_CASE(duplicate_fd_wait_does_not_remove_original_waiter) {
    int fds[2];
    CHECK_EQ(::pipe(fds), 0);
    bool awakened = false;
    bool rejected = false;
    Scheduler scheduler;
    Task first = wait_on_pipe(fds[0], awakened);
    scheduler.spawn(first);
    scheduler.spawn(duplicate_pipe_wait(fds[0], rejected));
    scheduler.spawn(signal_pipe(fds[1], first.handle));
    scheduler.run();
    ::close(fds[0]);
    ::close(fds[1]);
    CHECK_TRUE(rejected);
    CHECK_TRUE(awakened);
}

TEST_CASE(invalid_fd_wait_reports_registration_error) {
    bool rejected = false;
    Scheduler scheduler;
    scheduler.spawn(duplicate_pipe_wait(-1, rejected));
    scheduler.run();
    CHECK_TRUE(rejected);
}

namespace {

struct RecordDestruction {
    std::vector<int>& order;
    int id;
    ~RecordDestruction() { order.push_back(id); }
};

Task suspended_child(std::vector<int>& order) {
    RecordDestruction guard{order, 2};
    co_await sleep_for(60);
}

Task suspended_parent(std::vector<int>& order) {
    RecordDestruction guard{order, 1};
    co_await suspended_child(order);
}

Task suspended_background(std::vector<int>& order) {
    RecordDestruction guard{order, 3};
    co_await sleep_for(60);
}

Task owner_spawning_background(Scheduler& scheduler, Task::handle_type& child, std::vector<int>& order) {
    RecordDestruction guard{order, 4};
    Task task = suspended_background(order);
    child = task.handle;
    scheduler.spawn(task);
    co_await sleep_for(60);
}

}  // namespace

TEST_CASE(shutdown_destroys_suspended_tasks_and_children_in_dependency_order) {
    std::vector<int> order;
    Scheduler scheduler;
    scheduler.spawn(suspended_parent(order));
    scheduler.spawn(suspended_background(order));
    scheduler.run_ready();
    CHECK_EQ(scheduler.live_count(), static_cast<size_t>(2));
    scheduler.shutdown();
    CHECK_TRUE(order == std::vector<int>({3, 2, 1}));
    CHECK_EQ(scheduler.live_count(), static_cast<size_t>(0));
    scheduler.shutdown();
    CHECK_TRUE(order == std::vector<int>({3, 2, 1}));
}

TEST_CASE(explicit_shutdown_dependency_overrides_spawn_order) {
    std::vector<int> order;
    Scheduler scheduler;
    Task dependent = suspended_parent(order);
    Task prerequisite = suspended_background(order);
    scheduler.spawn(dependent);
    scheduler.spawn(prerequisite);
    scheduler.add_shutdown_dependency(dependent.handle, prerequisite.handle);
    scheduler.run_ready();
    scheduler.shutdown();
    CHECK_TRUE(order == std::vector<int>({2, 1, 3}));
}

TEST_CASE(shutdown_dependency_rejects_cycles_and_non_live_tasks) {
    std::vector<int> order;
    Scheduler scheduler;
    Task first = suspended_parent(order);
    Task second = suspended_background(order);
    scheduler.spawn(first);
    scheduler.spawn(second);
    scheduler.add_shutdown_dependency(first.handle, second.handle);
    CHECK_THROWS(scheduler.add_shutdown_dependency(second.handle, first.handle), RuntimeErrorKeyloom);
    CHECK_THROWS(scheduler.add_shutdown_dependency(first.handle, first.handle), RuntimeErrorKeyloom);
    scheduler.run_ready();
    scheduler.shutdown();
    CHECK_TRUE(order == std::vector<int>({2, 1, 3}));
    CHECK_THROWS(scheduler.add_shutdown_dependency(first.handle, second.handle), RuntimeErrorKeyloom);
}

TEST_CASE(tasks_spawned_during_execution_depend_on_their_spawning_root) {
    std::vector<int> order;
    Task::handle_type child{};
    Scheduler scheduler;
    Task owner = owner_spawning_background(scheduler, child, order);
    scheduler.spawn(owner);
    scheduler.run_ready();
    CHECK_THROWS(scheduler.add_shutdown_dependency(owner.handle, child), RuntimeErrorKeyloom);
    scheduler.shutdown();
    CHECK_TRUE(order == std::vector<int>({3, 4}));
}

namespace {

Task use_shared_resource(std::shared_ptr<int> resource, int& result) {
    co_await sleep_for(0.01);
    result = *resource;
}

Task short_lived_owner(std::weak_ptr<int>& observed, int& result) {
    auto resource = std::make_shared<int>(42);
    observed = resource;
    Scheduler::current().spawn(use_shared_resource(resource, result));
    co_return;
}

}  // namespace

TEST_CASE(spawned_task_can_retain_resource_after_owner_finishes) {
    std::weak_ptr<int> observed;
    int result = 0;
    Scheduler scheduler;
    scheduler.spawn(short_lived_owner(observed, result));
    scheduler.run_ready();
    CHECK_TRUE(!observed.expired());
    CHECK_EQ(scheduler.live_count(), static_cast<size_t>(1));
    scheduler.run();
    CHECK_EQ(result, 42);
    CHECK_TRUE(observed.expired());
}

TEST_CASE(fatal_epoll_error_destroys_tasks_and_is_reported) {
    std::vector<int> order;
    Scheduler scheduler([](int, epoll_event*, int, int) {
        errno = EIO;
        return -1;
    });
    scheduler.spawn(suspended_parent(order));
    CHECK_THROWS(scheduler.run(), RuntimeErrorKeyloom);
    CHECK_TRUE(order == std::vector<int>({2, 1}));
    CHECK_EQ(scheduler.live_count(), static_cast<size_t>(0));
    CHECK_THROWS(Scheduler::current(), RuntimeErrorKeyloom);
}
