#pragma once

#include "net/frame_assembler.hpp"
#include "net/protocol.hpp"
#include "net/socket.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace exchange::net {

/// Readiness interfaces the reactor dispatches through.
///
/// **The legitimate case for multiple inheritance**, and worth contrasting
/// with the case that gives it its reputation. Both are pure interfaces: no
/// data members, no constructors, no implementation to inherit. There is no
/// diamond, nothing to duplicate, and no ambiguity about which base a field
/// came from -- the problems that make multiple inheritance of *state* a bad
/// idea simply do not arise.
///
/// The alternative, one `IEventHandler` with both methods, would force every
/// read-only or write-only participant to implement a method it has no
/// meaning for. Separating them lets the reactor say precisely what it needs.
class IReader {
public:
    IReader() = default;
    IReader(const IReader&) = delete;
    IReader& operator=(const IReader&) = delete;
    IReader(IReader&&) = delete;
    IReader& operator=(IReader&&) = delete;
    virtual ~IReader() = default;

    /// The socket has data, or has reached end of stream.
    virtual void onReadable() = 0;
};

class IWriter {
public:
    IWriter() = default;
    IWriter(const IWriter&) = delete;
    IWriter& operator=(const IWriter&) = delete;
    IWriter(IWriter&&) = delete;
    IWriter& operator=(IWriter&&) = delete;
    virtual ~IWriter() = default;

    /// The socket can accept more bytes.
    virtual void onWritable() = 0;
};

/// One client connection.
///
/// **Lifetime.** Held by `shared_ptr`, because a session is referenced by the
/// reactor's registration table *and* by anything with work in flight for it
/// -- the publisher fanning out a fill, a worker part way through decoding.
/// A raw pointer would make "the client disconnected while a fill was being
/// written" a use-after-free; ownership by whoever closes it makes the same
/// event a dangling pointer somewhere else.
///
/// `enable_shared_from_this` lets a member function hand out a `shared_ptr` to
/// itself so an in-flight operation can keep the object alive until it
/// finishes. Constructing `std::shared_ptr<Session>(this)` instead would build
/// a *second, independent* control block: two owners each believing they are
/// the only one, and a double free when both reach zero. That is the failure
/// to be able to describe.
///
/// The subscription list holds `weak_ptr` rather than `shared_ptr`, so a
/// dropped client is detected by a failed `lock()` instead of being kept alive
/// forever by the very list that was supposed to notify it.
///
/// **Threading.** `onReadable` and `onWritable` run only on the reactor
/// thread. `send` is called from the engine thread, so the outbound buffer has
/// its own mutex -- the one place in the network layer that needs one, and
/// uncontended in practice because the two threads touch it at different
/// points in the cycle.
class Session final : public IReader, public IWriter, public std::enable_shared_from_this<Session> {
public:
    /// Invoked on the reactor thread for each complete inbound frame.
    using FrameHandler = std::function<void(const std::shared_ptr<Session>&, const Frame&)>;

    /// Invoked when the session has been closed, so the reactor can drop its
    /// registration.
    using CloseHandler = std::function<void(const std::shared_ptr<Session>&)>;

    /// Private-constructor factory: `enable_shared_from_this` only works if
    /// the object is genuinely owned by a `shared_ptr`, so stack allocation
    /// has to be impossible rather than merely discouraged.
    [[nodiscard]] static std::shared_ptr<Session> create(Fd socket, std::size_t maxOutbound,
                                                         int sendBufferBytes = 0);

    ~Session() override = default;

    void setFrameHandler(FrameHandler handler) { onFrame_ = std::move(handler); }

    void setCloseHandler(CloseHandler handler) { onClose_ = std::move(handler); }

    void onReadable() override;

    void onWritable() override;

    /// Queues bytes for delivery. Safe to call from any thread.
    ///
    /// @return false if the outbound buffer would exceed its bound. The
    ///         session is marked for closure and the caller should stop
    ///         writing to it.
    [[nodiscard]] bool send(std::span<const std::byte> bytes);

    /// True once the peer has gone or the session has been dropped.
    ///
    /// Atomic because send() consults it from the engine thread while the
    /// reactor thread may be setting it. A plain bool here would be a data
    /// race on the one flag the whole lifetime story depends on.
    [[nodiscard]] bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

    /// Whether the reactor should watch for writability -- i.e. whether
    /// anything is queued. Checked under the outbound lock.
    [[nodiscard]] bool wantsWrite() const;

    [[nodiscard]] int fd() const noexcept { return socket_.get(); }

    [[nodiscard]] std::size_t outboundBytes() const;

    /// Bytes dropped because the buffer was full. Non-zero means a client was
    /// disconnected for being too slow. Read under the outbound lock, since
    /// that is where it is written.
    [[nodiscard]] std::size_t overflowed() const;

    void close() noexcept;

private:
    Session(Fd socket, std::size_t maxOutbound);

    /// Drains the socket to EAGAIN and dispatches every frame that completes.
    void readAvailable();

    /// Writes as much of the outbound buffer as the socket accepts.
    void flush();

    Fd socket_;
    FrameAssembler assembler_;
    FrameHandler onFrame_;
    CloseHandler onClose_;

    /// Guards the outbound buffer against the engine thread queuing a fill
    /// while the reactor thread is draining it.
    mutable std::mutex outboundMutex_;
    std::deque<std::byte> outbound_;
    std::size_t maxOutbound_;
    std::size_t overflowed_{0};

    std::atomic<bool> closed_{false};
};

} // namespace exchange::net
