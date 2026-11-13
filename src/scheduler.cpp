#include "scheduler.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/epoll.h>
#include <utility>
#include <unistd.h>

namespace loomwright {
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
    std::coroutine_handle<promise_type> frame) noexcept {
    Scheduler* owner = frame.promise().scheduler;
    if (owner != nullptr) owner->notify_finished(frame);
    const handle_type awaiting = frame.promise().continuation;
    if (!awaiting) {
        frame.destroy();
        return std::noop_coroutine();
    }
    // Hand the awaiting frame back to the scheduler instead of resuming it
    // inline. An inline resume runs the awaiter on top of this frame's stack,
    // so a script that awaits once per loop iteration grows the native stack
    // without bound and eventually overflows it. The scheduler resumes the
    // awaiter from its own loop, so the stack stays flat.
    Scheduler* runner = awaiting.promise().driver;
    if (runner == nullptr) runner = frame.promise().driver;
    if (runner != nullptr) {
        runner->defer_resume(awaiting);
        return std::noop_coroutine();
    }
    return awaiting;
}

bool Task::Awaiter::await_ready() const noexcept { return !handle || handle.done(); }

std::coroutine_handle<Task::promise_type> Task::Awaiter::await_suspend(
    std::coroutine_handle<Task::promise_type> awaiting) noexcept {
    handle.promise().continuation = awaiting;
    handle.promise().parent = awaiting;
    awaiting.promise().child = handle;
    // An awaited child is not a spawned task, but the scheduler still has to know
    // the frame exists so a pending timer for it is not pruned.
    Scheduler* driver = awaiting.promise().driver;
    if (driver == nullptr) driver = &Scheduler::current();
    handle.promise().driver = driver;
    driver->register_task(handle);
    handle.promise().started = true;
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
    // The frame was cancelled while it was parked, waiting for the scheduler to
    // resume it. It cannot observe the cancellation any other way.
    if (parent && parent.promise().cancel_pending) {
        parent.promise().cancel_pending = false;
        throw TaskCancelled{};
    }
}

Scheduler::Scheduler(WaitFunction wait) : wait_(std::move(wait)) {
    if (!wait_) wait_ = ::epoll_wait;
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) throw RuntimeErrorLoomwright("cannot create an epoll instance");
}

Scheduler::~Scheduler() {
    shutdown();
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
    if (g_current == this) g_current = nullptr;
}

Scheduler& Scheduler::current() {
    if (g_current == nullptr) throw RuntimeErrorLoomwright("no scheduler is running");
    return *g_current;
}

double Scheduler::now() const {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Scheduler::spawn(Task task) {
    if (!task.handle) return;
    task.handle.promise().scheduler = this;
    task.handle.promise().driver = this;
    known_.insert(task.handle);
    live_.insert(task.handle);
    live_order_.push_back(task.handle);
    if (active_root_ && is_live(active_root_)) add_shutdown_dependency(task.handle, active_root_);
    ready_.push_back(task.handle);
}

void Scheduler::defer_resume(handle_type handle) {
    if (!handle) return;
    // A frame can be handed back more than once (a cancel and a completion
    // race), so keep the queue free of duplicates.
    if (std::find(deferred_.begin(), deferred_.end(), handle) != deferred_.end()) return;
    deferred_.push_back(handle);
    ++deferred_resume_count_;
}

void Scheduler::register_task(handle_type handle) {
    if (handle) known_.insert(handle);
}

void Scheduler::forget_task(handle_type handle) {
    known_.erase(handle);
    std::erase(ready_, handle);
    std::erase(deferred_, handle);
    remove_waits(handle);
    for (auto entry = fd_waiters_.begin(); entry != fd_waiters_.end();) {
        if (entry->second == handle) {
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, entry->first, nullptr);
            entry = fd_waiters_.erase(entry);
        } else {
            ++entry;
        }
    }
}

void Scheduler::remove_waits(handle_type handle) {
    timer_waits_.erase(handle);
    std::erase(stop_waiters_, handle);
    std::erase(drain_waiters_, handle);
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
    while (deepest.promise().child) {
        const handle_type child = deepest.promise().child;
        if (known_.find(child) == known_.end()) break;
        // A finished child cannot be woken; the frame that waits for it is the
        // one that has to be resumed.
        if (child.done()) break;
        deepest = child;
    }
    for (handle_type frame = handle;; frame = frame.promise().child) {
        frame.promise().cancelled = true;
        remove_waits(frame);
        if (frame == deepest) break;
    }
    // A stop or fd notification may already have queued this suspension. A
    // second wake would resume whatever the coroutine awaits next, prematurely.
    std::erase(ready_, deepest);
    if (std::find(deferred_.begin(), deferred_.end(), deepest) != deferred_.end()) {
        // The frame is parked: the scheduler resumes it from its own queue, so
        // the await has to report the cancellation when that happens.
        deepest.promise().cancel_pending = true;
        return;
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
    std::erase(ready_, handle);
    remove_waits(handle);
    live_.erase(handle);
    std::erase(live_order_, handle);
    remove_dependencies(handle);
    if (live_.size() <= 1 && !drain_waiters_.empty()) {
        for (handle_type waiter : drain_waiters_) {
            if (known_.find(waiter) != known_.end()) ready_.push_back(waiter);
        }
        drain_waiters_.clear();
    }
}

void Scheduler::suspend_until(handle_type handle, double seconds) {
    const double deadline = now() + (seconds > 0.0 ? seconds : 0.0);
    const std::uint64_t sequence = timer_sequence_++;
    timer_waits_[handle] = sequence;
    timers_.push(Timer{deadline, sequence, handle});
}

int Scheduler::suspend_on_fd(handle_type handle, int fd) {
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) < 0) {
        return errno;
    }
    fd_waiters_[fd] = handle;
    return 0;
}

void Scheduler::release_fd(int fd, handle_type owner) {
    const auto found = fd_waiters_.find(fd);
    if (found == fd_waiters_.end() || found->second != owner) return;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    fd_waiters_.erase(found);
}

void ReadableAwaiter::await_resume() {
    scheduler->release_fd(fd, handle);
    scheduler->check_cancelled(handle);
    if (registration_error != 0) {
        throw RuntimeErrorLoomwright("cannot wait for fd " + std::to_string(fd) + ": " +
                                  std::strerror(registration_error));
    }
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

void Scheduler::resume_frame(handle_type handle) {
    // A spawned task is live_; an awaited child is only known_. Both are resumable.
    if (known_.find(handle) == known_.end()) [[unlikely]] return;
    // A frame that already reached its final suspend point must not be resumed.
    if (handle.done()) [[unlikely]] return;
    if (!handle.promise().started && handle.promise().cancelled) {
        notify_finished(handle);
        handle.destroy();
        return;
    }
    handle.promise().started = true;
    const handle_type previous = active_root_;
    // Awaited children transfer directly without returning to the scheduler;
    // follow their parent links to associate spawned tasks with the live root.
    handle_type root = handle;
    while (root.promise().parent) root = root.promise().parent;
    active_root_ = root;
    handle.resume();
    active_root_ = previous;
}

void Scheduler::resume(handle_type handle) {
    resume_frame(handle);
    // A frame that finished while this one ran handed its awaiter back. Resume
    // those from this loop instead of from the finished frame's stack, so the
    // native stack does not grow with the number of awaits a script performs.
    drain_deferred();
}

void Scheduler::drain_deferred() {
    // Resume the frames that were handed back while the current one ran. Each
    // of them may hand back further frames, so keep draining until the queue
    // is empty. Resuming here rather than from `resume` keeps the stack flat.
    while (!deferred_.empty()) {
        const handle_type handle = deferred_.front();
        deferred_.pop_front();
        resume_frame(handle);
    }
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

void Scheduler::run_ready() {
    Scheduler* previous = g_current;
    g_current = this;
    pump();
    g_current = previous;
}

void Scheduler::add_shutdown_dependency(handle_type dependent, handle_type prerequisite) {
    if (!is_live(dependent) || !is_live(prerequisite))
        throw RuntimeErrorLoomwright("shutdown dependencies require live spawned tasks");
    std::vector<handle_type> pending{prerequisite};
    std::set<handle_type, HandleLess> visited;
    while (!pending.empty()) {
        const handle_type current = pending.back();
        pending.pop_back();
        if (current == dependent) throw RuntimeErrorLoomwright("cyclic shutdown dependency");
        if (!visited.insert(current).second) continue;
        const auto found = shutdown_dependencies_.find(current);
        if (found != shutdown_dependencies_.end())
            pending.insert(pending.end(), found->second.begin(), found->second.end());
    }
    shutdown_dependencies_[dependent].insert(prerequisite);
}

void Scheduler::remove_dependencies(handle_type handle) {
    shutdown_dependencies_.erase(handle);
    for (auto it = shutdown_dependencies_.begin(); it != shutdown_dependencies_.end();) {
        it->second.erase(handle);
        if (it->second.empty()) it = shutdown_dependencies_.erase(it);
        else ++it;
    }
}

void Scheduler::shutdown() {
    request_stop();
    Scheduler* previous = g_current;
    g_current = this;
    while (!live_order_.empty()) {
        auto selected = live_order_.end();
        for (auto candidate = live_order_.rbegin(); candidate != live_order_.rend(); ++candidate) {
            bool has_dependents = false;
            for (const auto& entry : shutdown_dependencies_) {
                if (entry.second.count(*candidate) != 0) {
                    has_dependents = true;
                    break;
                }
            }
            if (!has_dependents) {
                selected = std::prev(candidate.base());
                break;
            }
        }
        // The public registration method rejects cycles; retain a defensive
        // fallback so a destructor can still complete if an invariant breaks.
        if (selected == live_order_.end()) selected = std::prev(live_order_.end());
        const handle_type handle = *selected;
        live_order_.erase(selected);
        live_.erase(handle);
        remove_dependencies(handle);
        destroy_chain(*this, handle);
    }
    g_current = previous;
}

void Scheduler::run() {
    Scheduler* previous = g_current;
    g_current = this;
    while (!live_.empty()) {
        pump();
        if (live_.empty()) break;

        while (!timers_.empty()) {
            const Timer& timer = timers_.top();
            const auto wait = timer_waits_.find(timer.handle);
            if (wait != timer_waits_.end() && wait->second == timer.sequence) break;
            timers_.pop();
        }

        int timeout_ms = -1;
        if (!timers_.empty()) {
            const double delta = timers_.top().deadline - now();
            timeout_ms = delta <= 0.0 ? 0 : static_cast<int>(std::ceil(delta * 1000.0));
        }
        if (!ready_.empty()) timeout_ms = 0;

        epoll_event events[64];
        const int count = wait_(epoll_fd_, events, 64, timeout_ms);
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
            const int error = errno;
            shutdown();
            g_current = previous;
            throw RuntimeErrorLoomwright("epoll_wait failed: " + std::string(std::strerror(error)));
        }

        const double time_now = now();
        while (!timers_.empty() && timers_.top().deadline <= time_now) {
            const Timer timer = timers_.top();
            timers_.pop();
            const auto wait = timer_waits_.find(timer.handle);
            if (wait != timer_waits_.end() && wait->second == timer.sequence) {
                timer_waits_.erase(wait);
                ready_.push_back(timer.handle);
            }
        }
    }
    g_current = previous;
}

}  // namespace loomwright
