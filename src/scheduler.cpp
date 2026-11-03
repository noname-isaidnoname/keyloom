#include "scheduler.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sys/epoll.h>
#include <unistd.h>

namespace keyloom {
namespace {

Scheduler* g_current = nullptr;

/// Destroy a suspended frame and everything it is still awaiting. `forget_task`
/// runs first so stale timers and fd waiters for the whole chain are dropped.
void destroy_chain(Scheduler& scheduler, Task::handle_type frame) {
    if (!frame) return;
    const Task::handle_type child = frame.promise().child;
    frame.promise().child = {};
    scheduler.forget_task(frame);
    if (child) {
        child.promise().parent = {};
        destroy_chain(scheduler, child);
    }
    frame.destroy();
}

}  // namespace

Task Task::promise_type::get_return_object() noexcept {
    return Task{handle_type::from_promise(*this)};
}

std::coroutine_handle<> Task::promise_type::FinalAwaiter::await_suspend(
    std::coroutine_handle<promise_type> handle) noexcept {
    Scheduler* scheduler = handle.promise().scheduler;
    if (scheduler != nullptr) scheduler->notify_finished(handle);
    std::coroutine_handle<> continuation = handle.promise().continuation;
    if (!continuation) {
        handle.destroy();
        return std::noop_coroutine();
    }
    // Hand control straight back to the awaiting coroutine; it destroys the frame
    // after reading the stored exception.
    return continuation;
}

bool Task::Awaiter::await_ready() const noexcept { return !handle || handle.done(); }

std::coroutine_handle<> Task::Awaiter::await_suspend(std::coroutine_handle<> awaiting) noexcept {
    handle.promise().continuation = awaiting;
    handle.promise().parent = handle_type::from_address(awaiting.address());
    handle.promise().parent.promise().child = handle;
    // An awaited child is not a spawned task, but the scheduler still has to know
    // the frame exists so a pending timer for it is not pruned.
    Scheduler::current().register_task(handle);
    return handle;
}

void Task::Awaiter::await_resume() {
    if (!handle) return;
    const handle_type frame = handle;
    handle = {};
    Scheduler& scheduler = Scheduler::current();
    const handle_type parent = frame.promise().parent;
    frame.promise().parent = {};
    if (parent) parent.promise().child = {};
    if (!frame.done()) {
        // The child was cancelled while suspended. Destroying the frame runs its
        // local destructors (releasing taps and holds) and abandons the rest of
        // the body, exactly like a cancelled asyncio task. Any frame it is still
        // awaiting has to go with it, or it would resume into freed memory.
        scheduler.forget_task(frame);
        destroy_chain(scheduler, frame);
        throw TaskCancelled{};
    }
    // Read the error before destroying the frame; a nested failure has to surface
    // in the awaiting coroutine so its own handlers can run.
    std::exception_ptr failure = frame.promise().error;
    scheduler.forget_task(frame);
    frame.destroy();
    if (failure) std::rethrow_exception(failure);
}

Scheduler::Scheduler() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) throw RuntimeErrorKeyloom("cannot create an epoll instance");
}

Scheduler::~Scheduler() {
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
    if (g_current == this) g_current = nullptr;
}

Scheduler& Scheduler::current() {
    if (g_current == nullptr) throw RuntimeErrorKeyloom("no scheduler is running");
    return *g_current;
}

double Scheduler::now() const {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Scheduler::spawn(Task task) {
    if (!task.handle) return;
    task.handle.promise().scheduler = this;
    known_.insert(task.handle);
    live_.insert(task.handle);
    ready_.push_back(task.handle);
}

void Scheduler::register_task(handle_type handle) {
    if (handle) known_.insert(handle);
}

void Scheduler::forget_task(handle_type handle) {
    known_.erase(handle);
    for (auto entry = fd_waiters_.begin(); entry != fd_waiters_.end();) {
        if (entry->second == handle) {
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, entry->first, nullptr);
            entry = fd_waiters_.erase(entry);
        } else {
            ++entry;
        }
    }
}

// Awaited child tasks are tracked in known_ but not live_ (only spawned tasks
// are), so waiters may be either; known_ is the resumability predicate.
void Scheduler::request_stop() {
    stop_ = true;
    for (handle_type handle : stop_waiters_) {
        if (known_.find(handle) != known_.end()) ready_.push_back(handle);
    }
    stop_waiters_.clear();
}

void Scheduler::cancel(handle_type handle) {
    if (!handle) return;
    if (known_.find(handle) == known_.end()) return;
    if (handle.promise().cancelled) return;
    // A task suspended on `co_await child()` cannot be resumed directly: its
    // resume point is the await expression, so resuming it would destroy the
    // child frame while the grandchild chain is still running and leave the
    // grandchild's continuation pointing into freed memory. Wake the frame that
    // is actually suspended, and mark the whole chain so each frame unwinds with
    // TaskCancelled at its own await point.
    handle_type deepest = handle;
    while (deepest.promise().child && known_.find(deepest.promise().child) != known_.end()) {
        deepest = deepest.promise().child;
    }
    for (handle_type frame = handle;; frame = frame.promise().child) {
        frame.promise().cancelled = true;
        if (frame == deepest) break;
    }
    ready_.push_back(deepest);
}

void Scheduler::cancel_all() {
    std::vector<handle_type> living(live_.begin(), live_.end());
    for (handle_type handle : living) cancel(handle);
}

void Scheduler::check_cancelled(handle_type handle) {
    if (handle && handle.promise().cancelled) throw TaskCancelled{};
}

void Scheduler::notify_finished(handle_type handle) {
    known_.erase(handle);
    live_.erase(handle);
    if (live_.size() <= 1 && !drain_waiters_.empty()) {
        for (handle_type waiter : drain_waiters_) {
            if (known_.find(waiter) != known_.end()) ready_.push_back(waiter);
        }
        drain_waiters_.clear();
    }
}

void Scheduler::suspend_until(handle_type handle, double seconds) {
    const double deadline = now() + (seconds > 0.0 ? seconds : 0.0);
    timers_.push(Timer{deadline, timer_sequence_++, handle});
}

void Scheduler::suspend_on_fd(handle_type handle, int fd) {
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) < 0) {
        // Already registered (or a dead fd): fall back to an immediate resume so
        // the reader's own read() reports the real error.
        ready_.push_back(handle);
        return;
    }
    fd_waiters_[fd] = handle;
}

void Scheduler::release_fd(int fd) {
    const auto found = fd_waiters_.find(fd);
    if (found == fd_waiters_.end()) return;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    fd_waiters_.erase(found);
}

void Scheduler::wait_for_stop(handle_type handle) {
    if (stop_) {
        ready_.push_back(handle);
        return;
    }
    stop_waiters_.push_back(handle);
}

void Scheduler::wait_for_drain(handle_type handle) {
    if (live_.size() <= 1) {
        ready_.push_back(handle);
        return;
    }
    drain_waiters_.push_back(handle);
}

void Scheduler::resume(handle_type handle) {
    // A spawned task is live_; an awaited child is only known_. Both are resumable.
    if (known_.find(handle) == known_.end()) return;
    handle.resume();
}

void Scheduler::pump() {
    size_t guard = 0;
    while (!ready_.empty()) {
        handle_type handle = ready_.front();
        ready_.pop_front();
        if (known_.find(handle) == known_.end()) continue;
        resume(handle);
        // A pathological task that only awaits zero-duration work must not lock
        // out fd polling forever.
        if (++guard > 100000) {
            guard = 0;
            break;
        }
    }
}

void Scheduler::run() {
    Scheduler* previous = g_current;
    g_current = this;
    while (!live_.empty()) {
        pump();
        if (live_.empty()) break;

        while (!timers_.empty() && known_.find(timers_.top().handle) == known_.end()) timers_.pop();

        int timeout_ms = -1;
        if (!timers_.empty()) {
            const double delta = timers_.top().deadline - now();
            timeout_ms = delta <= 0.0 ? 0 : static_cast<int>(std::ceil(delta * 1000.0));
        }
        if (!ready_.empty()) timeout_ms = 0;

        epoll_event events[64];
        const int count = ::epoll_wait(epoll_fd_, events, 64, timeout_ms);
        if (count > 0) {
            for (int i = 0; i < count; ++i) {
                const int fd = events[i].data.fd;
                const auto found = fd_waiters_.find(fd);
                if (found == fd_waiters_.end()) continue;
                handle_type handle = found->second;
                fd_waiters_.erase(found);
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                if (known_.find(handle) != known_.end()) ready_.push_back(handle);
            }
        } else if (count < 0 && errno != EINTR) {
            // Nothing else can make progress; leave the loop rather than spin.
            break;
        }

        const double time_now = now();
        while (!timers_.empty() && timers_.top().deadline <= time_now) {
            const Timer timer = timers_.top();
            timers_.pop();
            if (known_.find(timer.handle) != known_.end()) ready_.push_back(timer.handle);
        }
    }
    g_current = previous;
}

}  // namespace keyloom
