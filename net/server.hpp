#pragma once

#include "concurrent/thread_pool.hpp"
#include "core/engine.hpp"
#include "core/fill.hpp"
#include "core/types.hpp"
#include "net/protocol.hpp"
#include "net/reactor.hpp"
#include "net/session.hpp"
#include "net/socket.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace exchange::net {

struct ServerConfig {
    std::uint16_t port{0}; ///< 0 lets the kernel pick, which is what tests use.
    std::size_t workerThreads{2};
    std::size_t commandQueueCapacity{1024};
    std::size_t workerQueueCapacity{1024};

    /// Per-session outbound cap. Exceeding it disconnects the client rather
    /// than growing memory on the exchange's side.
    std::size_t maxOutboundBytes{256 * 1024};

    /// Kernel send-buffer cap per connection; 0 leaves the default. Used by
    /// the tests to make the outbound bound reachable on loopback.
    int socketSendBufferBytes{0};
};

/// Ties the reactor, the worker pool and the engine together.
///
/// Thread layout, matching ARCHITECTURE section 2:
///
/// | Thread   | Count | Owns                       | Touches the book? |
/// |----------|-------|----------------------------|-------------------|
/// | Reactor  | 1     | epoll set, socket buffers  | No                |
/// | Worker   | N     | nothing durable            | No                |
/// | Engine   | 1     | the entire Book            | **Exclusively**   |
///
/// Bytes flow reactor to workers to engine, and fills flow back out through
/// the sessions. The book is never locked, because only the engine thread ever
/// reaches it.
///
/// **Teardown order matters and is the part that deadlocks if got wrong**:
/// stop the reactor so no new work is accepted, join the workers so nothing
/// else can be pushed, then stop the engine so it drains what it already has.
/// Stopping the engine first would leave workers blocked on a queue nobody
/// will drain.
class Server {
public:
    explicit Server(ServerConfig config);

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    ~Server();

    /// Binds, starts the engine and the reactor thread, and begins accepting.
    void start();

    /// Stops everything in the order described above. Idempotent.
    void stop() noexcept;

    /// The port actually bound, which is only known after start() when the
    /// config asked for 0.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] std::size_t sessionCount() const;

    [[nodiscard]] std::size_t acceptedConnections() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t droppedForBackpressure() const noexcept {
        return droppedSlow_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const Engine& engine() const noexcept { return engine_; }

private:
    void acceptReady();
    void onFrame(const std::shared_ptr<Session>& session, const Frame& frame);
    void dispatch(const std::shared_ptr<Session>& session, ClientMessage&& message);
    void publishFill(const Fill& fill);

    /// Queues bytes and asks the reactor to send them.
    ///
    /// Callable from any thread. The write itself never happens here: only the
    /// reactor thread touches a socket, so this records the descriptor and
    /// wakes the loop. Writing directly would race the reactor draining the
    /// same buffer, and would put a syscall on the engine thread.
    void deliver(const std::shared_ptr<Session>& session, const std::vector<std::byte>& bytes);

    /// Runs on the reactor thread after a wake: flushes sockets with queued
    /// bytes and retires sessions that were closed from elsewhere.
    void servicePending();

    ServerConfig config_;
    Engine engine_;
    ThreadPool workers_;
    Reactor reactor_;
    Fd listener_;
    std::thread reactorThread_;
    std::uint16_t port_{0};

    /// Sessions the reactor is currently serving, keyed by descriptor.
    /// Reactor thread only.
    std::unordered_map<int, std::shared_ptr<Session>> sessions_;

    /// The fill fan-out list, reachable from the engine thread.
    ///
    /// **`weak_ptr`, deliberately.** A `shared_ptr` here would keep a
    /// disconnected client alive for as long as anyone remembered to publish
    /// to it -- the subscription list becoming the thing that prevents the
    /// cleanup it was supposed to trigger. A failed `lock()` is how a dropped
    /// client is noticed.
    mutable std::mutex subscribersMutex_;
    std::vector<std::weak_ptr<Session>> subscribers_;

    /// Maps an account to the session that owns it, so a fill reaches the
    /// participant rather than being broadcast. Guarded by the same mutex.
    std::unordered_map<AccountId, std::weak_ptr<Session>> byAccount_;

    /// Descriptors with queued output, and descriptors to unregister.
    ///
    /// Both exist because sessions_ and the epoll set are reactor-thread-only.
    /// A worker closing a session must not erase from them directly -- it would
    /// be mutating the reactor state underneath the loop.
    mutable std::mutex pendingMutex_;
    std::vector<int> needFlush_;
    std::vector<int> needClose_;

    std::atomic<std::size_t> accepted_{0};
    std::atomic<std::size_t> droppedSlow_{0};
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
};

} // namespace exchange::net
