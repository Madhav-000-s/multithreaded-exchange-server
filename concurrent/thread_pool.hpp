#pragma once

#include "concurrent/task.hpp"
#include "concurrent/thread_safe_queue.hpp"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace exchange {

/// A fixed pool of workers draining one bounded queue.
///
/// In the exchange this is where decoding, validation and account resolution
/// happen -- work that is CPU-bound, independent per message, and must not run
/// on the reactor thread. Workers never touch the order book; they push
/// commands toward the single engine thread.
///
/// Shutdown is the part worth reading. `stop()` closes the queue, which wakes
/// every worker blocked in `pop()` and every producer blocked in `push()`, and
/// then joins. Workers drain what is already queued before exiting, so work
/// already accepted is not silently dropped. The destructor calls `stop()`, so
/// a pool cannot be destroyed with threads still running -- a `std::thread`
/// destroyed while joinable calls `std::terminate`.
class ThreadPool {
public:
    /// @param threads  worker count; clamped to at least one.
    /// @param capacity queue bound, which is what applies backpressure to
    ///        whoever is submitting.
    ThreadPool(std::size_t threads, std::size_t capacity);

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    ~ThreadPool();

    /// Blocks while the queue is full, which is the backpressure.
    /// @return false if the pool is stopping; the task is not run.
    [[nodiscard]] bool submit(Task task);

    /// Enqueues only if there is room right now.
    [[nodiscard]] bool trySubmit(Task task);

    /// Closes the queue, lets workers drain it, then joins. Idempotent and
    /// safe to call from any thread, including from the destructor.
    void stop() noexcept;

    [[nodiscard]] std::size_t threadCount() const noexcept { return workers_.size(); }

    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }

    /// Tasks that ran to completion.
    [[nodiscard]] std::size_t completed() const noexcept {
        return completed_.load(std::memory_order_relaxed);
    }

    /// Tasks that escaped an exception. A worker survives one -- see the
    /// catch-all in the run loop.
    [[nodiscard]] std::size_t failed() const noexcept {
        return failed_.load(std::memory_order_relaxed);
    }

private:
    void run() noexcept;

    ThreadSafeQueue<Task> queue_;
    std::vector<std::thread> workers_;

    /// Counters only, never read to make a decision, so relaxed ordering is
    /// sufficient: there is nothing for the read to be ordered *against*.
    /// Anything used for control flow would need acquire/release.
    std::atomic<std::size_t> completed_{0};
    std::atomic<std::size_t> failed_{0};

    /// Guards stop() against being run twice concurrently. seq_cst by default
    /// and deliberately left that way: shutdown happens once, so the cost is
    /// irrelevant and the simplest correct ordering is the right one.
    std::atomic<bool> stopping_{false};
};

} // namespace exchange
