#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/epoll.h>

#include "errors.hpp"

namespace loomwright {

class Scheduler;

/// A coroutine driven by the Scheduler. A spawned Task is detached: its frame is
/// destroyed at the final suspend point. A task that is `co_await`ed directly
/// hands control to its caller instead and is destroyed by the awaiter.
struct Task {
    struct promise_type {
        /// The scheduler that spawned this frame. Only spawned frames report
        /// their completion to it.
        Scheduler* scheduler = nullptr;
        /// The scheduler that drives this frame. It is set for spawned frames
        /// and for awaited children. A frame that finishes hands its awaiter
        /// back through this scheduler rather than resuming it inline, so a chain
        /// of awaits cannot grow the native stack.
        Scheduler* driver = nullptr;
        bool cancelled = false;
        /// Set when a frame that was handed back to the scheduler is cancelled.
        /// The frame is parked at its await point, so it cannot observe the
        /// cancellation until the scheduler resumes it again.
        bool cancel_pending = false;
        bool started = false;
        /// True while the frame has an entry in `Scheduler::ready_` or
        /// `Scheduler::deferred_`. Lets a push deduplicate in O(1) instead of
        /// scanning the queue. Cleared when the frame is popped. A frame that
        /// is already finished and destroyed is never dereferenced: pops check
        /// `known_` first.
        bool ready_queued = false;
        bool deferred_queued = false;
        std::exception_ptr error;
        /// The frame that awaits this one. It is handed back to the driver
        /// instead of resumed inline, so a chain of awaits cannot grow the
        /// native stack.
        std::coroutine_handle<promise_type> continuation{};
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
        std::coroutine_handle<promise_type> await_suspend(std::coroutine_handle<promise_type> awaiting) noexcept;
        void await_resume();
    };
    Awaiter operator co_await() const noexcept { return Awaiter{handle}; }
};

struct HandleLess {
    bool operator()(Task::handle_type left, Task::handle_type right) const {
        return left.address() < right.address();
    }
};

/// The hash of a coroutine frame. The frames are identified by their
/// address, so the hash is the address itself.
struct HandleHash {
    std::size_t operator()(Task::handle_type handle) const noexcept {
        return std::hash<const void*>{}(handle.address());
    }
};

/// One epoll-based event loop for devices, timers and dispatch tasks. It gives
/// .loom scripts cooperative ordering without an interpreter in the hot path.
class Scheduler {
public:
    using handle_type = Task::handle_type;

    using WaitFunction = std::function<int(int, epoll_event*, int, int)>;
    explicit Scheduler(WaitFunction wait = {});
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    static Scheduler& current();

    /// Tasks spawned by a running root depend on that root during shutdown.
    /// Dependencies constrain forced destruction, not ordinary task completion;
    /// detached tasks must own resources they use beyond their caller's lifetime.
    void spawn(Task task);
    void run();
    /// Run currently ready tasks without waiting for timers or descriptors.
    void run_ready();
    /// Declare that dependent must be destroyed before prerequisite at shutdown.
    /// Both must be live spawned tasks; cycles are rejected.
    void add_shutdown_dependency(handle_type dependent, handle_type prerequisite);
    /// Destroy dependents before prerequisites; unrelated tasks use reverse spawn order.
    void shutdown();

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

    /// Hand a frame back to the scheduler so it is resumed from the
    /// scheduler's own loop instead of inline from the frame that finished.
    /// A frame that is resumed inline keeps its caller's stack frame alive, so
    /// a script that awaits once per loop iteration would grow the native
    /// stack without bound and eventually overflow it.
    void defer_resume(handle_type handle);

    /// How many frames were handed back to the scheduler instead of being
    /// resumed inline. Every completed await goes through this path, so the
    /// counter shows that continuations are driven from the scheduler's loop.
    std::size_t deferred_resume_count() const { return deferred_resume_count_; }

    void suspend_until(handle_type handle, double seconds);
    int suspend_on_fd(handle_type handle, int fd);
    void release_fd(int fd, handle_type owner);
    void wait_for_stop(handle_type handle);
    void wait_for_drain(handle_type handle);
    void check_cancelled(handle_type handle);
    void notify_finished(handle_type handle);

private:
    void pump();
    void push_ready(handle_type handle);
    /// Resume a frame once, without draining the deferred queue.
    void resume_frame(handle_type handle);
    void resume(handle_type handle);
    void remove_waits(handle_type handle);
    void remove_dependencies(handle_type handle);
    /// Resume every frame that was handed back while the current one ran.
    void drain_deferred();

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
    // A timer belongs to one particular suspension, not merely a frame address.
    std::unordered_map<handle_type, std::uint64_t, HandleHash> timer_waits_;
    std::unordered_map<int, handle_type> fd_waiters_;
    std::unordered_set<handle_type, HandleHash> live_;
    std::vector<handle_type> live_order_;
    std::map<handle_type, std::set<handle_type, HandleLess>, HandleLess> shutdown_dependencies_;
    std::unordered_set<handle_type, HandleHash> known_;
    handle_type active_root_{};
    std::deque<handle_type> deferred_;
    std::size_t deferred_resume_count_ = 0;
    std::vector<handle_type> stop_waiters_;
    std::vector<handle_type> drain_waiters_;
    int epoll_fd_ = -1;
    bool stop_ = false;
    WaitFunction wait_;
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
    int registration_error = 0;

    bool await_ready() const noexcept { return false; }
    bool await_suspend(Task::handle_type suspended) noexcept {
        handle = suspended;
        registration_error = scheduler->suspend_on_fd(suspended, fd);
        return registration_error == 0;
    }
    void await_resume();
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

/// Drain used by cleanup: cancellation may wake it early, so report whether the
/// drain actually completed instead of throwing and abandoning cleanup.
struct CleanupDrainAwaiter {
    Scheduler* scheduler;
    bool await_ready() const noexcept { return false; }
    void await_suspend(Task::handle_type suspended) noexcept { scheduler->wait_for_drain(suspended); }
    bool await_resume() const { return scheduler->live_count() <= 1; }
};

inline CleanupDrainAwaiter cleanup_drain() { return CleanupDrainAwaiter{&Scheduler::current()}; }

}  // namespace loomwright
