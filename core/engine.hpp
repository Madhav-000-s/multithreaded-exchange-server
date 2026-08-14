#pragma once

#include "concurrent/thread_safe_queue.hpp"
#include "core/book.hpp"
#include "core/command.hpp"
#include "core/fill.hpp"
#include "core/types.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace exchange {

/// The single thread that owns the order book.
///
/// **The book is never locked, because exactly one thread ever touches it.**
/// That is the most important sentence in the design. Every other thread
/// reaches the book by pushing a Command into a bounded queue; the engine
/// drains that queue serially and is the sole owner of everything downstream.
///
/// The alternative -- a mutex around the book, taken by every worker -- would
/// be simpler to draw and worse in every respect that matters. Matching is not
/// parallelisable anyway, because price-time priority is a total order over
/// arrivals, so a lock would serialise the same work while adding contention,
/// convoying, and a class of bug (a forgotten lock) that message passing makes
/// unrepresentable.
///
/// Callbacks fire **on the engine thread**, so a handler must not block: doing
/// so stalls matching for every participant. Handlers that need to do real
/// work should hand off to their own queue.
class Engine {
public:
    using FillHandler = std::function<void(const Fill&)>;

    /// @param capacity command queue bound, and therefore the backpressure
    ///        point between the network side and matching.
    explicit Engine(std::size_t capacity);

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    /// Stops and joins. Safe to destroy without an explicit stop().
    ~Engine();

    /// Installs the fill callback. Must be called before start(): afterwards
    /// the engine thread reads it without synchronisation, which is only
    /// sound because it stops changing.
    void onFill(FillHandler handler);

    /// Launches the engine thread. Idempotent.
    void start();

    /// Closes the queue, lets the engine drain what was already accepted, then
    /// joins. Idempotent and callable from any thread.
    void stop() noexcept;

    /// Blocks while the queue is full.
    /// @return false if the engine is stopping; the command is dropped.
    [[nodiscard]] bool submit(Command command);

    [[nodiscard]] bool trySubmit(Command command);

    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }

    /// Commands applied to the book.
    [[nodiscard]] std::size_t processed() const noexcept {
        return processed_.load(std::memory_order_relaxed);
    }

    /// Commands rejected by the book -- an invalid order, an unknown id. Not
    /// an error in the engine; a normal outcome that is counted rather than
    /// thrown out of the thread.
    [[nodiscard]] std::size_t rejected() const noexcept {
        return rejected_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t fills() const noexcept {
        return fills_.load(std::memory_order_relaxed);
    }

    /// The book itself.
    ///
    /// **Only safe once the engine has been joined.** Handing out a reference
    /// to state another thread mutates would undo the whole ownership model,
    /// so this exists for tests and for post-shutdown inspection. Phase 5
    /// publishes market data through the L2 snapshot instead, which is what a
    /// reader should use while the engine is live.
    [[nodiscard]] const Book& bookAfterShutdown() const noexcept { return book_; }

private:
    void run() noexcept;
    void apply(Command&& command);

    Book book_;
    ThreadSafeQueue<Command> queue_;
    std::thread thread_;
    FillHandler onFill_;

    /// Read by other threads to observe liveness, so it is published with
    /// release and read with acquire -- the store must not be reordered before
    /// the thread is actually up.
    std::atomic<bool> running_{false};

    /// Guards start()/stop() against concurrent or repeated calls. seq_cst,
    /// which is the default, and deliberately not tuned: this happens once per
    /// process lifetime, so the simplest correct ordering is the right one.
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};

    /// Statistics only. Relaxed is sufficient because nothing branches on
    /// them; there is no other memory whose visibility they need to order.
    std::atomic<std::size_t> processed_{0};
    std::atomic<std::size_t> rejected_{0};
    std::atomic<std::size_t> fills_{0};
};

} // namespace exchange
