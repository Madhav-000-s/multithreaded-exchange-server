#include "concurrent/thread_pool.hpp"

#include "concurrent/task.hpp"

#include <cstddef>
#include <optional>
#include <utility>

namespace exchange {

ThreadPool::ThreadPool(std::size_t threads, std::size_t capacity) : queue_(capacity) {
    const std::size_t count = threads == 0 ? 1 : threads;
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this] { run(); });
    }
}

ThreadPool::~ThreadPool() {
    // A std::thread destroyed while still joinable calls std::terminate, so
    // the destructor must join rather than merely hope the caller did.
    stop();
}

bool ThreadPool::submit(Task task) {
    return queue_.push(std::move(task));
}

bool ThreadPool::trySubmit(Task task) {
    return queue_.tryPush(std::move(task));
}

void ThreadPool::stop() noexcept {
    // exchange() rather than load-then-store: two threads calling stop()
    // concurrently must not both proceed to join, because joining an already
    // joined thread is undefined.
    if (stopping_.exchange(true)) {
        return;
    }

    // Closing wakes workers blocked in pop() and producers blocked in push().
    // Queued work is still drained first -- close() refuses new items, it does
    // not discard accepted ones.
    queue_.close();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::run() noexcept {
    // pop() returns nullopt only when the queue is closed *and* empty, so this
    // loop is the entire termination condition. There is no flag polled
    // alongside it and no sentinel value to recognise.
    while (std::optional<Task> task = queue_.pop()) {
        try {
            (*task)();
            completed_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            // An exception escaping a thread entry point is std::terminate,
            // so the catch-all is not defensive programming -- it is the only
            // thing standing between one bad task and the whole process.
            //
            // Deliberately swallowed rather than rethrown: a worker that dies
            // on a malformed message shrinks the pool permanently, and by the
            // tenth such message there is no pool left. The count is exposed
            // so the failure is visible rather than silent.
            failed_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace exchange
