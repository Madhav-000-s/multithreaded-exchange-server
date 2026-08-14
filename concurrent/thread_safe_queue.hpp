#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

namespace exchange {

/// A bounded multi-producer, single-consumer queue with a clean shutdown.
///
/// **Why bounded.** An unbounded queue converts a throughput problem into a
/// memory problem: if producers outrun the engine, the queue absorbs the
/// difference until the process is killed by the OOM killer, which is the
/// worst possible failure mode for a system whose next act is to persist
/// state. A bound turns that into backpressure -- producers block, the network
/// layer stops reading, and TCP flow control pushes the problem back to the
/// client, where it belongs.
///
/// **Why a mutex here and nowhere else.** The whole design minimises shared
/// state by message passing rather than by locking, and this queue is the one
/// place the two sides genuinely meet. Everything downstream of it is owned
/// exclusively by the engine thread and needs no lock at all.
///
/// **The shutdown flag is a plain bool, not an atomic.** That is deliberate
/// and is the crux of the lost-wakeup problem:
///
///   - It is only ever read or written while the mutex is held, so it needs no
///     atomicity of its own.
///   - It is checked *inside* the condition-variable predicate. A waiter
///     evaluates the predicate under the lock before blocking, so a close()
///     that lands between "decided to wait" and "actually waiting" cannot be
///     missed -- the two are one atomic step as far as the condvar is
///     concerned.
///
/// Making it a `std::atomic<bool>` checked outside the lock is the classic way
/// to reintroduce exactly the race the predicate exists to remove: the waiter
/// tests the flag, close() sets it and notifies, and only then does the waiter
/// block -- forever, because the notification has already been delivered.
///
/// There is also no sentinel-value hack. Pushing N poison pills for N
/// consumers requires knowing N, breaks if a consumer dies, and cannot wake a
/// producer blocked on a full queue at all.
///
/// @tparam T element type; must be movable. Copyability is not required, so
///         the queue can carry `unique_ptr`-owning commands.
template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue(ThreadSafeQueue&&) = delete;
    ThreadSafeQueue& operator=(ThreadSafeQueue&&) = delete;
    ~ThreadSafeQueue() = default;

    /// Blocks until there is room or the queue closes.
    /// @return false if the queue was closed; the value is not enqueued.
    [[nodiscard]] bool push(T value) {
        {
            std::unique_lock<std::mutex> lock(mutex_);

            // The predicate form, not the bare wait(). It re-checks on every
            // wake, so a spurious wakeup simply loops -- and it is evaluated
            // once before blocking, which is what closes the lost-wakeup
            // window described above.
            notFull_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });

            if (closed_) {
                return false;
            }
            items_.push(std::move(value));
        }
        // Notify after releasing the lock: a consumer woken while the producer
        // still holds the mutex would immediately block on it again.
        notEmpty_.notify_one();
        return true;
    }

    /// Enqueues only if there is room right now.
    /// @return false if the queue is full or closed.
    [[nodiscard]] bool tryPush(T value) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || items_.size() >= capacity_) {
                return false;
            }
            items_.push(std::move(value));
        }
        notEmpty_.notify_one();
        return true;
    }

    /// Blocks until an item is available, or until the queue is closed *and*
    /// drained.
    ///
    /// Note the ordering: closing does not discard what is already queued.
    /// The consumer keeps receiving until the backlog is gone, which is what
    /// lets the engine finish the work it has accepted before shutting down.
    [[nodiscard]] std::optional<T> pop() {
        std::optional<T> value;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            notEmpty_.wait(lock, [this] { return closed_ || !items_.empty(); });

            if (items_.empty()) {
                return std::nullopt; // closed and drained: the only exit
            }
            value = std::move(items_.front());
            items_.pop();
        }
        notFull_.notify_one();
        return value;
    }

    /// Dequeues only if an item is available right now.
    [[nodiscard]] std::optional<T> tryPop() {
        std::optional<T> value;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (items_.empty()) {
                return std::nullopt;
            }
            value = std::move(items_.front());
            items_.pop();
        }
        notFull_.notify_one();
        return value;
    }

    /// Refuses further pushes and wakes everyone blocked on either condvar.
    ///
    /// Idempotent, so a shutdown path that closes twice is not a bug. Both
    /// condition variables are notified because producers and consumers wait
    /// on different ones and both must be released -- notifying only the
    /// consumer side is a shutdown that hangs with producers still blocked.
    ///
    /// notify_all, not notify_one: every waiter needs to observe the flag, and
    /// waking one leaves the rest asleep forever.
    void close() noexcept {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    [[nodiscard]] bool closed() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    /// Instantaneous size. Inherently stale the moment it returns, so it is
    /// for tests and metrics, never for control flow.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.empty();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::queue<T> items_;
    std::size_t capacity_;
    bool closed_{false};
};

} // namespace exchange
