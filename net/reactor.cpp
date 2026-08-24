#include "net/reactor.hpp"

#include "core/exceptions.hpp"
#include "net/socket.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace exchange::net {
namespace {

constexpr int kMaxEventsPerWait = 64;

[[nodiscard]] std::string describe(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

} // namespace

Reactor::Reactor()
    : epoll_(::epoll_create1(EPOLL_CLOEXEC)), wakeup_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (!epoll_.valid()) {
        throw NetworkError(describe("epoll_create1"));
    }
    if (!wakeup_.valid()) {
        throw NetworkError(describe("eventfd"));
    }

    // The wakeup descriptor is registered like any other, so shutdown arrives
    // through the same path as a client event and needs no special case in the
    // loop.
    add(wakeup_.get(), EPOLLIN | EPOLLET, [this](std::uint32_t) { drainWakeup(); });
}

void Reactor::add(int fd, std::uint32_t events, ReadyHandler handler) {
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;

    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &event) != 0) {
        throw NetworkError(describe("epoll_ctl(ADD)"));
    }
    handlers_[fd] = std::move(handler);
}

void Reactor::modify(int fd, std::uint32_t events) {
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;

    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, fd, &event) != 0) {
        throw NetworkError(describe("epoll_ctl(MOD)"));
    }
}

void Reactor::remove(int fd) noexcept {
    // Failure is ignored on purpose: closing a descriptor removes it from the
    // interest set automatically, so a remove() after close legitimately
    // reports ENOENT or EBADF and is not an error worth propagating from a
    // teardown path.
    (void)::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, nullptr);
    handlers_.erase(fd);
}

void Reactor::run() {
    running_.store(true, std::memory_order_release);
    std::array<epoll_event, kMaxEventsPerWait> events{};

    while (!stopping_.load(std::memory_order_acquire)) {
        const int ready = ::epoll_wait(epoll_.get(), events.data(), kMaxEventsPerWait, -1);

        if (ready < 0) {
            if (errno == EINTR) {
                continue; // a signal, not a failure
            }
            running_.store(false, std::memory_order_release);
            throw NetworkError(describe("epoll_wait"));
        }

        for (int i = 0; i < ready; ++i) {
            const int fd = events[static_cast<std::size_t>(i)].data.fd;
            const std::uint32_t mask = events[static_cast<std::size_t>(i)].events;

            // Looked up rather than stored in epoll_data, because a handler
            // may close and unregister other descriptors -- including ones
            // whose events are already in this batch. Finding nothing means
            // the descriptor was retired mid-batch, and skipping it is the
            // correct response rather than a stale-pointer dereference.
            const auto handler = handlers_.find(fd);
            if (handler == handlers_.end()) {
                continue;
            }
            // Copied before the call: the callback can erase entries from
            // handlers_, which would invalidate the iterator underneath it.
            const ReadyHandler callback = handler->second;
            callback(mask);
        }
    }

    running_.store(false, std::memory_order_release);
}

void Reactor::wake() noexcept {
    const std::uint64_t one = 1;
    // The result is checked rather than cast to void: glibc marks write()
    // warn_unused_result, and a (void) cast does not suppress it. A failure
    // here means the counter is already saturated, which is itself a pending
    // wakeup, so there is nothing to do about it.
    const ssize_t written = ::write(wakeup_.get(), &one, sizeof(one));
    static_cast<void>(written);
}

void Reactor::stop() noexcept {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // Nudge the eventfd so a loop parked in epoll_wait with an infinite
    // timeout observes the flag. Setting the flag alone would leave it asleep
    // until a client happened to send something.
    // Nudge the loop so one parked in epoll_wait with an infinite timeout
    // observes the flag rather than sleeping until a client happens to send.
    wake();
}

void Reactor::drainWakeup() noexcept {
    // Edge-triggered, so the counter must be read to zero or no further write
    // will produce an event.
    std::uint64_t value = 0;
    while (::read(wakeup_.get(), &value, sizeof(value)) == sizeof(value)) {
        // keep draining
    }
    if (onWake_) {
        onWake_();
    }
}

} // namespace exchange::net
