#include "net/session.hpp"

#include "core/exceptions.hpp"
#include "net/frame_assembler.hpp"
#include "net/protocol.hpp"
#include "net/socket.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace exchange::net {
namespace {

/// One read syscall's worth. Large enough that a burst of small orders
/// arrives in a single call, small enough to sit on the stack.
constexpr std::size_t kReadChunk = 16 * 1024;

/// Session's constructor is private, so make_shared cannot reach it. This
/// adaptor gives make_shared a public constructor to call while keeping the
/// real one closed -- one allocation for the object and its control block
/// together, instead of the two that shared_ptr(new Session(...)) would take.
struct MakeSharedSession;

} // namespace

std::shared_ptr<Session> Session::create(Fd socket, std::size_t maxOutbound, int sendBufferBytes) {
    setNonBlocking(socket.get());
    setNoDelay(socket.get());
    if (sendBufferBytes > 0) {
        setSendBufferSize(socket.get(), sendBufferBytes);
    }

    // new + shared_ptr rather than make_shared because the constructor is
    // private. The pointer is wrapped on the very next token, which is the
    // factory exemption ARCHITECTURE section 5 allows.
    return std::shared_ptr<Session>(new Session(std::move(socket), maxOutbound));
}

Session::Session(Fd socket, std::size_t maxOutbound)
    : socket_(std::move(socket)), assembler_(kMaxFrameBody), maxOutbound_(maxOutbound) {}

void Session::onReadable() {
    if (closed()) {
        return;
    }
    readAvailable();
}

void Session::onWritable() {
    if (closed()) {
        return;
    }
    flush();
}

void Session::readAvailable() {
    // Keep a reference to ourselves alive for the duration. A frame handler
    // may close this session -- the reactor then drops its shared_ptr, and
    // without this local the object would be destroyed underneath the loop
    // that is still running in it.
    const std::shared_ptr<Session> self = shared_from_this();

    std::array<std::byte, kReadChunk> chunk{};

    // **Edge-triggered epoll reports readiness once per transition**, so the
    // socket must be drained until it says EAGAIN. Reading once per event --
    // which is correct under level-triggered -- silently strands whatever did
    // not fit in that first read: epoll never reports it again, and the
    // connection hangs holding data nobody will look at.
    while (true) {
        const ssize_t got = ::recv(socket_.get(), chunk.data(), chunk.size(), 0);

        if (got > 0) {
            assembler_.append(
                std::span<const std::byte>(chunk.data(), static_cast<std::size_t>(got)));

            try {
                while (const std::optional<Frame> frame = assembler_.next()) {
                    if (onFrame_) {
                        onFrame_(self, *frame);
                    }
                    if (closed()) {
                        return; // a handler dropped us
                    }
                }
            } catch (const ProtocolError&) {
                // The framing itself is wrong, so there is no way to resync:
                // every subsequent byte would be interpreted at the wrong
                // offset. Dropping the session is the only honest response,
                // and is why ProtocolError is a distinct type from
                // InvalidOrderError, which rejects one message and continues.
                close();
                return;
            }
            continue;
        }

        if (got == 0) {
            close(); // orderly shutdown by the peer
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; // drained: the only correct exit from an ET read loop
        }
        if (errno == EINTR) {
            continue; // interrupted by a signal, not an error
        }
        close();
        return;
    }
}

bool Session::send(std::span<const std::byte> bytes) {
    if (closed()) {
        return false;
    }

    {
        const std::lock_guard<std::mutex> lock(outboundMutex_);

        // **Backpressure, and the reason a slow client cannot hurt anyone
        // else.** Without a bound, a client that stops reading turns every
        // fill it is entitled to into retained memory, and one such client
        // eventually exhausts the process. Blocking the engine instead would
        // be worse still: matching for every participant would stall on the
        // slowest consumer.
        //
        // So the buffer is capped and the session is dropped. A client too
        // slow to keep up is the client's problem; making it the exchange's
        // problem is how one bad participant takes down a venue.
        if (outbound_.size() + bytes.size() > maxOutbound_) {
            overflowed_ += bytes.size();
            closed_.store(true, std::memory_order_release);
            return false;
        }

        outbound_.insert(outbound_.end(), bytes.begin(), bytes.end());
    }
    return true;
}

void Session::flush() {
    const std::shared_ptr<Session> self = shared_from_this();

    while (true) {
        std::vector<std::byte> chunk;
        {
            const std::lock_guard<std::mutex> lock(outboundMutex_);
            if (outbound_.empty()) {
                return;
            }
            // Copied out of the deque so the syscall runs without the lock
            // held. A send() that blocks the engine thread behind a socket
            // write would reintroduce exactly the coupling the bounded buffer
            // exists to prevent.
            const std::size_t take = std::min(outbound_.size(), kReadChunk);
            chunk.assign(outbound_.begin(), outbound_.begin() + static_cast<std::ptrdiff_t>(take));
        }

        // MSG_NOSIGNAL: writing to a socket whose peer has gone raises SIGPIPE
        // by default, which terminates the process. A disconnecting client
        // must not be able to kill the server.
        const ssize_t sent = ::send(socket_.get(), chunk.data(), chunk.size(), MSG_NOSIGNAL);

        if (sent > 0) {
            const std::lock_guard<std::mutex> lock(outboundMutex_);
            outbound_.erase(outbound_.begin(),
                            outbound_.begin() + static_cast<std::ptrdiff_t>(sent));
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return; // socket full; epoll will report writability again
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        close();
        return;
    }
}

bool Session::wantsWrite() const {
    const std::lock_guard<std::mutex> lock(outboundMutex_);
    return !outbound_.empty();
}

std::size_t Session::outboundBytes() const {
    const std::lock_guard<std::mutex> lock(outboundMutex_);
    return outbound_.size();
}

std::size_t Session::overflowed() const {
    const std::lock_guard<std::mutex> lock(outboundMutex_);
    return overflowed_;
}

void Session::close() noexcept {
    // exchange() so a double close cannot invoke the close handler twice --
    // the reactor would then erase a registration it had already replaced.
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (onClose_) {
        onClose_(shared_from_this());
    }
}

} // namespace exchange::net
