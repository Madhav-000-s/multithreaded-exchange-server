#pragma once

#include "net/socket.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace exchange::net {

/// Single-threaded epoll event loop, edge-triggered.
///
/// **Why epoll and not select.** `select` takes a bitmask of descriptors and
/// the kernel scans all of them on every call, so its cost is O(watched)
/// whether or not anything happened; it is also capped at `FD_SETSIZE`, which
/// is 1024. `epoll` keeps the interest set in the kernel across calls and
/// returns only the descriptors that are ready, so its cost is O(ready) and
/// the set is bounded only by the process descriptor limit. For a server
/// holding ten thousand mostly-idle connections that is the difference between
/// working and not.
///
/// **Why edge-triggered.** Level-triggered epoll re-reports a descriptor on
/// every wait for as long as it stays ready, so a socket with data nobody has
/// read yet wakes the loop repeatedly. Edge-triggered reports only the
/// transition, which is one wakeup per arrival instead of one per iteration.
///
/// The price is a contract the handler must honour: **drain until EAGAIN.**
/// Whatever is left unread when the handler returns will not be announced
/// again, and the connection stalls holding data. This is the single most
/// common way edge-triggered epoll is got wrong, and it is why every socket
/// registered here is non-blocking -- on a blocking socket the final read that
/// should report EAGAIN blocks instead, stalling the whole loop.
class Reactor {
public:
    using ReadyHandler = std::function<void(std::uint32_t events)>;

    Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;
    Reactor(Reactor&&) = delete;
    Reactor& operator=(Reactor&&) = delete;
    ~Reactor() = default;

    /// Registers a descriptor. `events` should already include EPOLLET.
    void add(int fd, std::uint32_t events, ReadyHandler handler);

    /// Changes the interest set for an existing descriptor -- used to start
    /// and stop watching for writability as the outbound buffer fills and
    /// drains.
    void modify(int fd, std::uint32_t events);

    /// Unregisters. Safe for a descriptor already closed, whose registration
    /// the kernel drops automatically.
    void remove(int fd) noexcept;

    /// Runs until `stop()`. Call from one thread only.
    void run();

    /// Makes the loop run one more iteration, from any thread.
    ///
    /// The reason this is public: work queued for a socket by another thread
    /// needs the reactor to notice. Edge-triggered EPOLLOUT reports the
    /// transition to writable, which on a socket that never fills happens once
    /// at registration and never again -- so anything queued afterwards would
    /// sit unsent forever. The eventfd is how a non-reactor thread says "there
    /// is something to do".
    void wake() noexcept;

    /// Runs on the reactor thread after each wake, before the next wait.
    /// Set before run().
    void setWakeHandler(std::function<void()> handler) { onWake_ = std::move(handler); }

    /// Asks the loop to exit, from any thread.
    ///
    /// Writing the flag is not enough: the loop may be parked in
    /// `epoll_wait` with no timeout and nothing else due to arrive. An
    /// `eventfd` registered alongside the sockets gives shutdown a descriptor
    /// to make readable, so the wakeup goes through the same mechanism as
    /// every other event rather than relying on a signal or a poll timeout.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    [[nodiscard]] std::size_t watched() const noexcept { return handlers_.size(); }

private:
    void drainWakeup() noexcept;

    Fd epoll_;
    Fd wakeup_;
    std::unordered_map<int, ReadyHandler> handlers_;
    std::function<void()> onWake_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
};

} // namespace exchange::net
