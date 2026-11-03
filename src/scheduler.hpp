#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <queue>
#include <set>
#include <vector>

#include "errors.hpp"

namespace keyloom {

class Scheduler;

/// A coroutine driven by the Scheduler. A spawned Task is detached: its frame is
/// destroyed at the final suspend point. A task that is `co_await`ed directly
/// hands control to its caller instead and is destroyed by the awaiter.
struct Task {
    struct promise_type {
        Scheduler* scheduler = nullptr;
        bool cancelled = false;
        std::exception_ptr error;
        std::coroutine_handle<> continuation{};
        /// Coroutine this one is currently `co_await`ing, and the coroutine
        /// awaiting this one. Together they form the live chain that owns the
        /// frames, so cancellation can wake the frame that is actually
        /// suspended instead of resuming a parent mid-`co_await`.
        std::coroutine_handle<promise_type> child{};
        std::coroutine_handle<promise_type> parent{};

        Task get_return_object() noexcept;
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> handle) noexcept;
            void await_resume() const noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;
    handle_type handle{};

    // Defined in scheduler.cpp: the scheduler type is still incomplete here.
    struct Awaiter {
        handle_type handle{};

        bool await_ready() const noexcept;
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept;
        void await_resume();
    };

    Awaiter operator co_await() const noexcept { return Awaiter{handle}; }
};

struct HandleLess {
    bool operator()(Task::handle_type left, Task::handle_type right) const {
        return left.address() < right.address();
    }
};

/// One epoll-based event loop for devices, timers and dispatch tasks. It gives
/// .loom scripts the cooperative ordering of the original runtime without an
/// interpreter in the hot path.
class Scheduler {
public:
    using handle_type = Task::handle_type;

    Scheduler();
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    static Scheduler& current();

    void spawn(Task task);
    void run();

    bool stop_requested() const { return stop_; }
    void request_stop();
    double now() const;
    bool is_live(handle_type handle) const { return live_.find(handle) != live_.end(); }
    size_t live_count() const { return live_.size(); }

    void cancel(handle_type handle);
    void cancel_all();

    /// Register/forget a coroutine frame so stale timers and fd waiters can be
    /// told apart from live ones.
    void register_task(handle_type handle);
    void forget_task(handle_type handle);

    void suspend_until(handle_type handle, double seconds);
    void suspend_on_fd(handle_type handle, int fd);
    void release_fd(int fd);
    void wait_for_stop(handle_type handle);
    void wait_for_drain(handle_type handle);
    void check_cancelled(handle_type handle);
    void notify_finished(handle_type handle);

private:
    void pump();
    void resume(handle_type handle);

    struct Timer {
        double deadline = 0.0;
        std::uint64_t sequence = 0;
        handle_type handle{};
    };
    struct TimerLater {
        bool operator()(const Timer& left, const Timer& right) const {
            if (left.deadline != right.deadline) return left.deadline > right.deadline;
            return left.sequence > right.sequence;
        }
    };

    std::deque<handle_type> ready_;
    std::priority_queue<Timer, std::vector<Timer>, TimerLater> timers_;
    std::uint64_t timer_sequence_ = 0;
    std::map<int, handle_type> fd_waiters_;
    std::set<handle_type, HandleLess> live_;
    std::set<handle_type, HandleLess> known_;
    std::vector<handle_type> stop_waiters_;
    std::vector<handle_type> drain_waiters_;
    int epoll_fd_ = -1;
    bool stop_ = false;
};

/// Suspend a task for a wall-clock duration.
struct SleepAwaiter {
    Scheduler* scheduler;
    double seconds;
    Task::handle_type handle{};

    bool await_ready() const noexcept { return false; }
    void await_suspend(Task::handle_type suspended) noexcept {
        handle = suspended;
        scheduler->suspend_until(suspended, seconds);
    }
    void await_resume() { scheduler->check_cancelled(handle); }
};

inline SleepAwaiter sleep_for(double seconds) { return SleepAwaiter{&Scheduler::current(), seconds}; }

/// Suspend a task until an fd is readable.
struct ReadableAwaiter {
    Scheduler* scheduler;
    int fd;
    Task::handle_type handle{};

    bool await_ready() const noexcept { return false; }
    void await_suspend(Task::handle_type suspended) noexcept {
        handle = suspended;
        scheduler->suspend_on_fd(suspended, fd);
    }
    void await_resume() {
        scheduler->release_fd(fd);
        scheduler->check_cancelled(handle);
    }
};

inline ReadableAwaiter readable(int fd) { return ReadableAwaiter{&Scheduler::current(), fd}; }

/// Suspend until the runtime asks the scheduler to stop.
struct StopAwaiter {
    Scheduler* scheduler;
    Task::handle_type handle{};

    bool await_ready() const noexcept { return false; }
    void await_suspend(Task::handle_type suspended) noexcept {
        handle = suspended;
        scheduler->wait_for_stop(suspended);
    }
    void await_resume() {
        scheduler->check_cancelled(handle);
    }
};

inline StopAwaiter wait_stop() { return StopAwaiter{&Scheduler::current()}; }

/// Suspend until every other task has finished unwinding.
struct DrainAwaiter {
    Scheduler* scheduler;
    Task::handle_type handle{};

    bool await_ready() const noexcept { return false; }
    void await_suspend(Task::handle_type suspended) noexcept {
        handle = suspended;
        scheduler->wait_for_drain(suspended);
    }
    void await_resume() { scheduler->check_cancelled(handle); }
};

inline DrainAwaiter drain() { return DrainAwaiter{&Scheduler::current()}; }

}  // namespace keyloom
