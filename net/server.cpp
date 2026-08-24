#include "net/server.hpp"

#include "core/command.hpp"
#include "core/exceptions.hpp"
#include "core/order.hpp"
#include "core/types.hpp"
#include "net/protocol.hpp"
#include "net/session.hpp"
#include "net/socket.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>

namespace exchange::net {
namespace {

template <typename... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

template <typename... Handlers>
Overloaded(Handlers...) -> Overloaded<Handlers...>;

/// Builds the concrete order type the wire asked for.
///
/// The only place in the system that turns an untrusted byte into a
/// polymorphic object, so every field is validated here rather than trusted
/// downstream. A precondition would be exactly wrong: it compiles to
/// `__builtin_unreachable()` in release, which would hand the optimiser an
/// assumption a remote peer controls.
[[nodiscard]] std::unique_ptr<Order> buildOrder(const NewOrderMessage& message) {
    if (message.quantity == 0) {
        throw InvalidOrderError("quantity must be positive");
    }

    switch (message.kind) {
    case OrderKind::Limit:
        return std::make_unique<LimitOrder>(message.orderId, message.side, message.account,
                                            message.quantity, message.price);
    case OrderKind::Market:
        return std::make_unique<MarketOrder>(message.orderId, message.side, message.account,
                                             message.quantity);
    case OrderKind::Iceberg:
        if (message.displayQuantity == 0) {
            throw InvalidOrderError("an iceberg needs a display quantity");
        }
        return std::make_unique<IcebergOrder>(message.orderId, message.side, message.account,
                                              message.quantity, message.price,
                                              message.displayQuantity);
    }
    throw ProtocolError("unreachable order kind");
}

} // namespace

Server::Server(ServerConfig config)
    : config_(config),
      engine_(config.commandQueueCapacity),
      workers_(config.workerThreads, config.workerQueueCapacity) {}

Server::~Server() {
    stop();
}

void Server::start() {
    if (started_.exchange(true)) {
        return;
    }

    listener_ = listenOn(config_.port);
    port_ = boundPort(listener_.get());

    engine_.onFill([this](const Fill& fill) { publishFill(fill); });
    engine_.start();

    reactor_.setWakeHandler([this] { servicePending(); });

    // Level-triggered would be acceptable for the listener, but keeping every
    // descriptor edge-triggered means one contract rather than two: accept in
    // a loop until EAGAIN, exactly as sockets are read in a loop until EAGAIN.
    reactor_.add(listener_.get(), EPOLLIN | EPOLLET, [this](std::uint32_t) { acceptReady(); });

    reactorThread_ = std::thread([this] {
        try {
            reactor_.run();
        } catch (const std::exception&) {
            // An exception escaping a thread entry point is std::terminate.
            // The reactor dying stops new connections but leaves the engine
            // able to finish what it has.
        }
    });
}

void Server::stop() noexcept {
    if (stopping_.exchange(true)) {
        return;
    }

    // Order is the whole game. Reactor first, so nothing new is accepted or
    // read. Then the workers, so nothing further can be pushed at the engine.
    // Only then the engine, which drains what it already holds. Stopping the
    // engine first would leave workers blocked pushing into a queue nobody is
    // draining -- a shutdown that hangs.
    reactor_.stop();
    if (reactorThread_.joinable()) {
        reactorThread_.join();
    }

    workers_.stop();
    engine_.stop();

    sessions_.clear();
    {
        const std::lock_guard<std::mutex> lock(subscribersMutex_);
        subscribers_.clear();
        byAccount_.clear();
    }
    listener_.reset();
}

void Server::acceptReady() {
    // Edge-triggered: accept until EAGAIN. Accepting one connection per event
    // would strand every other client that arrived in the same instant, and
    // epoll would not mention them again.
    while (true) {
        const int client =
            ::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return; // backlog drained
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue; // a client vanished mid-handshake; not our problem
            }
            return;
        }

        std::shared_ptr<Session> session =
            Session::create(Fd(client), config_.maxOutboundBytes, config_.socketSendBufferBytes);

        session->setFrameHandler([this](const std::shared_ptr<Session>& owner, const Frame& frame) {
            onFrame(owner, frame);
        });
        session->setCloseHandler([this](const std::shared_ptr<Session>& owner) {
            // close() can be called from a worker or the engine thread, and
            // neither may touch the epoll set or the session map. Record the
            // descriptor and let the reactor retire it.
            {
                const std::lock_guard<std::mutex> lock(pendingMutex_);
                needClose_.push_back(owner->fd());
            }
            reactor_.wake();
        });

        const int fd = session->fd();
        reactor_.add(fd, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP,
                     [this, fd](std::uint32_t events) {
                         const auto entry = sessions_.find(fd);
                         if (entry == sessions_.end()) {
                             return;
                         }
                         // Copied so the connection survives its own close
                         // handler erasing the map entry mid-callback.
                         const std::shared_ptr<Session> ready = entry->second;

                         if ((events & (EPOLLHUP | EPOLLERR)) != 0) {
                             ready->close();
                             return;
                         }
                         if ((events & EPOLLOUT) != 0) {
                             ready->onWritable();
                         }
                         if ((events & (EPOLLIN | EPOLLRDHUP)) != 0) {
                             ready->onReadable();
                         }
                     });

        sessions_.emplace(fd, session);
        {
            const std::lock_guard<std::mutex> lock(subscribersMutex_);
            subscribers_.emplace_back(session);
        }
        accepted_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Server::onFrame(const std::shared_ptr<Session>& session, const Frame& frame) {
    // Decoded on the reactor thread because the payload is a view into the
    // session's buffer and does not outlive this call. Copying it to hand to a
    // worker would cost an allocation per message for work that is a few
    // shifts; the pool exists for the validation and account resolution that
    // follow, which is where the real cost is.
    ClientMessage message = decodeClientMessage(frame.type, frame.payload);

    // A shared_ptr, not a raw pointer: the client may disconnect between here
    // and the worker picking the task up, and the session has to outlive the
    // work in flight for it. This is the case enable_shared_from_this exists
    // to serve.
    const bool accepted =
        workers_.trySubmit([this, session, message = std::move(message)]() mutable {
            dispatch(session, std::move(message));
        });

    if (!accepted) {
        // The pool is saturated. Rejecting is the honest response: blocking
        // the reactor here would stall every other connection behind one
        // overloaded moment.
        const std::vector<std::byte> reject =
            encode(RejectedMessage{.orderId = 0, .reason = RejectReason::Overloaded});
        deliver(session, reject);
    }
}

void Server::dispatch(const std::shared_ptr<Session>& session, ClientMessage&& message) {
    try {
        std::visit(Overloaded{
                       [&](NewOrderMessage&& order) {
                           {
                               const std::lock_guard<std::mutex> lock(subscribersMutex_);
                               byAccount_[order.account] = session;
                           }
                           std::unique_ptr<Order> built = buildOrder(order);
                           if (!engine_.submit(SubmitCommand{.order = std::move(built)})) {
                               deliver(session,
                                       encode(RejectedMessage{.orderId = order.orderId,
                                                              .reason = RejectReason::Overloaded}));
                           }
                       },
                       [&](const CancelMessage& cancel) {
                           if (!engine_.submit(CancelCommand{.id = cancel.orderId})) {
                               deliver(session,
                                       encode(RejectedMessage{.orderId = cancel.orderId,
                                                              .reason = RejectReason::Overloaded}));
                           }
                       },
                       [&](const ModifyMessage& modify) {
                           if (!engine_.submit(ModifyCommand{.id = modify.orderId,
                                                             .quantity = modify.quantity,
                                                             .price = modify.price})) {
                               deliver(session,
                                       encode(RejectedMessage{.orderId = modify.orderId,
                                                              .reason = RejectReason::Overloaded}));
                           }
                       },
                   },
                   std::move(message));
    } catch (const InvalidOrderError&) {
        deliver(session,
                encode(RejectedMessage{.orderId = 0, .reason = RejectReason::InvalidOrder}));
    } catch (const ExchangeError&) {
        deliver(session, encode(RejectedMessage{.orderId = 0, .reason = RejectReason::Unknown}));
    }
}

void Server::publishFill(const Fill& fill) {
    // Runs on the engine thread. Both sides of the trade are notified, each
    // told the trade from its own perspective.
    const std::vector<std::byte> toAggressor =
        encode(FillMessage{.orderId = fill.aggressorId,
                           .counterpartyOrderId = fill.restingId,
                           .price = fill.price,
                           .quantity = fill.quantity,
                           .side = fill.aggressorSide});
    const std::vector<std::byte> toResting =
        encode(FillMessage{.orderId = fill.restingId,
                           .counterpartyOrderId = fill.aggressorId,
                           .price = fill.price,
                           .quantity = fill.quantity,
                           .side = opposite(fill.aggressorSide)});

    const std::lock_guard<std::mutex> lock(subscribersMutex_);

    const auto notify = [this](AccountId account, const std::vector<std::byte>& bytes) {
        const auto entry = byAccount_.find(account);
        if (entry == byAccount_.end()) {
            return;
        }
        // lock() failing is precisely how a departed client is detected. With
        // a shared_ptr in this map the session would still be here, and would
        // stay here.
        // lock() failing is precisely how a departed client is detected.
        if (const std::shared_ptr<Session> session = entry->second.lock()) {
            deliver(session, bytes);
        }
    };

    notify(fill.aggressorAccount, toAggressor);
    notify(fill.restingAccount, toResting);
}

void Server::deliver(const std::shared_ptr<Session>& session, const std::vector<std::byte>& bytes) {
    if (!session->send(bytes)) {
        // The outbound bound was hit: this client is not reading fast enough.
        droppedSlow_.fetch_add(1, std::memory_order_relaxed);
        session->close();
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(pendingMutex_);
        needFlush_.push_back(session->fd());
    }
    // Without this the bytes would sit in the buffer indefinitely. Edge
    // triggered EPOLLOUT reports the transition to writable, and a socket that
    // never fills transitions once -- at registration, when there was nothing
    // to send.
    reactor_.wake();
}

void Server::servicePending() {
    std::vector<int> flush;
    std::vector<int> close;
    {
        const std::lock_guard<std::mutex> lock(pendingMutex_);
        flush.swap(needFlush_);
        close.swap(needClose_);
    }

    // Reactor thread only, so sessions_ is safe to walk and mutate here.
    for (const int fd : flush) {
        const auto entry = sessions_.find(fd);
        if (entry != sessions_.end()) {
            entry->second->onWritable();
        }
    }
    for (const int fd : close) {
        reactor_.remove(fd);
        sessions_.erase(fd);
    }
}

std::size_t Server::sessionCount() const {
    const std::lock_guard<std::mutex> lock(subscribersMutex_);
    return static_cast<std::size_t>(
        std::count_if(subscribers_.begin(), subscribers_.end(),
                      [](const std::weak_ptr<Session>& weak) { return !weak.expired(); }));
}

} // namespace exchange::net
