#pragma once

#include <cstdint>
#include <utility>

namespace exchange::net {

/// An owning file descriptor.
///
/// A descriptor is a resource with exactly the same shape as memory: acquired,
/// released once, and catastrophic to release twice. `unique_ptr` does not
/// cover it because the sentinel is -1 rather than null and the release
/// operation is `close`, so it gets its own two dozen lines.
///
/// Leaking one is worse than leaking memory: the process has a hard limit on
/// open descriptors, so a leak per connection turns into `accept` failing with
/// EMFILE and the server refusing every client -- while looking perfectly
/// healthy in a memory profile.
class Fd {
public:
    Fd() noexcept = default;

    explicit Fd(int fd) noexcept : fd_(fd) {}

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    /// Hands the descriptor to the caller, who becomes responsible for it.
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    void reset() noexcept;

private:
    int fd_{-1};
};

/// Creates a listening socket bound to `port` on the loopback-inclusive
/// wildcard address.
///
/// Sets `SO_REUSEADDR`. Without it a restart within the TIME_WAIT window --
/// up to two minutes after the previous process exits -- fails to bind, so
/// every deploy or crash-restart hits it. The option lets a new socket bind a
/// port whose old connections are still winding down.
///
/// @param port 0 asks the kernel to allocate one, which is what the tests use
///        so they can run concurrently without colliding.
/// @throws NetworkError on any failure.
[[nodiscard]] Fd listenOn(std::uint16_t port, int backlog = 128);

/// The port a socket is actually bound to. Needed when the kernel chose it.
[[nodiscard]] std::uint16_t boundPort(int fd);

/// Connects to loopback. Used by the test client.
[[nodiscard]] Fd connectToLoopback(std::uint16_t port);

/// **Required for edge-triggered epoll, not merely advisable.**
///
/// Edge-triggered readiness is reported once per transition, so a handler must
/// keep reading until the syscall says EAGAIN. On a blocking socket that final
/// read does not return EAGAIN -- it blocks, and the single reactor thread
/// stops serving every other connection.
void setNonBlocking(int fd);

/// Disables Nagle's algorithm.
///
/// Nagle holds a small write back until either the previous packet is
/// acknowledged or enough data accumulates for a full segment. It exists to
/// stop telnet-style traffic filling the network with 41-byte packets, and it
/// is exactly wrong here: an order acknowledgement is small, latency-critical,
/// and has nothing following it to coalesce with. Combined with delayed ACK on
/// the far side it can add up to 40 ms to a message that took microseconds to
/// produce.
void setNoDelay(int fd);

/// Caps the kernel socket buffers.
///
/// Production use is to bound per-connection kernel memory across many
/// sessions. The tests use it to make backpressure reachable: on loopback the
/// default buffers are megabytes, so a client that never reads still absorbs
/// far more than any sane application-level bound, and the overflow path would
/// never be exercised. The kernel doubles the requested value and enforces a
/// floor, so these are hints rather than exact sizes.
void setSendBufferSize(int fd, int bytes);
void setReceiveBufferSize(int fd, int bytes);

} // namespace exchange::net
