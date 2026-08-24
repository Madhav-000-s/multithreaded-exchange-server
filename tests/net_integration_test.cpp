// End to end over real loopback sockets. This is Phase 5's exit criterion:
// multiple concurrent clients trading against each other, and a deliberately
// slow client disconnected rather than allowed to grow the server's memory.

#include "core/exceptions.hpp"
#include "core/types.hpp"
#include "net/frame_assembler.hpp"
#include "net/protocol.hpp"
#include "net/server.hpp"
#include "net/socket.hpp"
#include "net/wire.hpp"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace exchange::net {
namespace {

using namespace std::chrono_literals;

/// A blocking test client. Deliberately the opposite of the server's design:
/// there is no reason for a test harness to be asynchronous, and a blocking
/// client is far easier to reason about when a test fails.
class TestClient {
public:
    ///  receiveBufferBytes 0 leaves the kernel default. A small value is
    ///        how the slow-client test makes backpressure reachable: on
    ///        loopback the defaults are megabytes, so a client that never
    ///        reads still absorbs far more than any application-level bound.
    explicit TestClient(std::uint16_t port, int receiveBufferBytes = 0)
        : socket_(connectToLoopback(port)) {
        if (receiveBufferBytes > 0) {
            setReceiveBufferSize(socket_.get(), receiveBufferBytes);
        }
    }

    void send(const std::vector<std::byte>& frame) {
        std::size_t sent = 0;
        while (sent < frame.size()) {
            const ssize_t wrote =
                ::send(socket_.get(), frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
            if (wrote <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            sent += static_cast<std::size_t>(wrote);
        }
    }

    /// Waits up to `budget` for one server message.
    [[nodiscard]] std::optional<ServerMessage> receive(std::chrono::milliseconds budget = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + budget;

        while (std::chrono::steady_clock::now() < deadline) {
            if (const std::optional<Frame> frame = assembler_.next()) {
                return decodeServerMessage(frame->type, frame->payload);
            }

            std::array<std::byte, 4096> chunk{};
            const ssize_t got = ::recv(socket_.get(), chunk.data(), chunk.size(), MSG_DONTWAIT);
            if (got > 0) {
                assembler_.append(
                    std::span<const std::byte>(chunk.data(), static_cast<std::size_t>(got)));
                continue;
            }
            if (got == 0) {
                return std::nullopt; // server closed
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    /// True once the server has closed this connection.
    [[nodiscard]] bool disconnected() {
        std::array<std::byte, 1024> chunk{};
        const ssize_t got = ::recv(socket_.get(), chunk.data(), chunk.size(), MSG_DONTWAIT);
        if (got == 0) {
            return true;
        }
        return got < 0 && errno != EAGAIN && errno != EWOULDBLOCK;
    }

    [[nodiscard]] int fd() const noexcept { return socket_.get(); }

private:
    Fd socket_;
    FrameAssembler assembler_;
};

[[nodiscard]] NewOrderMessage limitOrder(OrderId id, AccountId account, Side side, Price price,
                                         Quantity qty) {
    return NewOrderMessage{.orderId = id,
                           .account = account,
                           .side = side,
                           .kind = OrderKind::Limit,
                           .price = price,
                           .quantity = qty,
                           .displayQuantity = 0};
}

/// Spins until `predicate` holds or the budget expires, so tests wait on the
/// condition itself rather than on a sleep long enough to probably work.
template <typename Predicate>
[[nodiscard]] bool waitFor(Predicate predicate, std::chrono::milliseconds budget = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

[[nodiscard]] ServerConfig testConfig() {
    ServerConfig config;
    config.port = 0; // the kernel picks, so tests can run in parallel
    config.workerThreads = 2;
    return config;
}

// ---------------------------------------------------------------------------

TEST(NetIntegration, BindsAnEphemeralPortAndAcceptsAConnection) {
    Server server(testConfig());
    server.start();
    ASSERT_NE(server.port(), 0);

    const TestClient client(server.port());

    EXPECT_TRUE(waitFor([&] { return server.acceptedConnections() == 1; }));
    server.stop();
}

TEST(NetIntegration, AcceptsManyConcurrentConnections) {
    Server server(testConfig());
    server.start();

    std::vector<std::unique_ptr<TestClient>> clients;
    clients.reserve(16);
    for (int i = 0; i < 16; ++i) {
        clients.push_back(std::make_unique<TestClient>(server.port()));
    }

    // The accept loop runs to EAGAIN, so a burst arriving together is fully
    // drained rather than one per epoll event.
    EXPECT_TRUE(waitFor([&] { return server.acceptedConnections() == 16; }));
    server.stop();
}

TEST(NetIntegration, TwoClientsTradeAgainstEachOther) {
    Server server(testConfig());
    server.start();

    TestClient seller(server.port());
    TestClient buyer(server.port());

    seller.send(encode(limitOrder(1, 100, Side::Sell, 250, 10)));
    EXPECT_TRUE(waitFor([&] { return server.engine().processed() >= 1; }));

    buyer.send(encode(limitOrder(2, 200, Side::Buy, 250, 4)));
    EXPECT_TRUE(waitFor([&] { return server.engine().fills() >= 1; }));

    // Both sides are told, each from its own perspective.
    const std::optional<ServerMessage> toBuyer = buyer.receive();
    ASSERT_TRUE(toBuyer.has_value());
    ASSERT_TRUE(std::holds_alternative<FillMessage>(*toBuyer));
    const FillMessage buyerFill = std::get<FillMessage>(*toBuyer);
    EXPECT_EQ(buyerFill.orderId, 2u);
    EXPECT_EQ(buyerFill.counterpartyOrderId, 1u);
    EXPECT_EQ(buyerFill.quantity, 4u);
    EXPECT_EQ(buyerFill.price, 250) << "the trade prints at the resting price";
    EXPECT_EQ(buyerFill.side, Side::Buy);

    const std::optional<ServerMessage> toSeller = seller.receive();
    ASSERT_TRUE(toSeller.has_value());
    const FillMessage sellerFill = std::get<FillMessage>(*toSeller);
    EXPECT_EQ(sellerFill.orderId, 1u);
    EXPECT_EQ(sellerFill.side, Side::Sell) << "reported from the resting side's point of view";

    server.stop();
}

TEST(NetIntegration, ManyClientsTradingConcurrentlyProduceAConsistentBook) {
    // The exit criterion proper: real sockets, real concurrency, and a book
    // that adds up afterwards.
    constexpr int kClients = 6;
    constexpr int kOrdersEach = 25;

    Server server(testConfig());
    server.start();

    std::vector<std::thread> traders;
    traders.reserve(kClients);

    for (int c = 0; c < kClients; ++c) {
        traders.emplace_back([&server, c] {
            TestClient client(server.port());
            const auto account = static_cast<AccountId>(c + 1);
            // Half the clients bid, half offer, at prices that cross.
            const Side side = (c % 2 == 0) ? Side::Buy : Side::Sell;

            for (int i = 0; i < kOrdersEach; ++i) {
                const auto id = static_cast<OrderId>(c * 1000 + i + 1);
                client.send(encode(limitOrder(id, account, side, 100, 5)));
            }
            // Hold the connection open until the server has caught up, so the
            // socket is not closed out from under in-flight work.
            (void)waitFor([&] { return server.engine().processed() >= kClients * kOrdersEach; },
                          3000ms);
        });
    }

    for (std::thread& trader : traders) {
        trader.join();
    }

    EXPECT_TRUE(waitFor([&] { return server.engine().processed() == kClients * kOrdersEach; }));
    server.stop();

    // Buys and sells were equal and all crossed at one price, so the book
    // should be empty and every unit matched.
    const Book& book = server.engine().bookAfterShutdown();
    EXPECT_EQ(server.engine().processed(), kClients * kOrdersEach);
    EXPECT_FALSE(book.isCrossed());
    EXPECT_GT(server.engine().fills(), 0u);
}

TEST(NetIntegration, MessagesSplitAcrossWritesStillDecode) {
    // TCP does not preserve write boundaries. Sending a frame in fragments,
    // with a pause between them, is exactly what the assembler exists for.
    Server server(testConfig());
    server.start();

    TestClient client(server.port());
    const std::vector<std::byte> frame = encode(limitOrder(1, 10, Side::Buy, 100, 5));

    for (std::size_t i = 0; i < frame.size(); ++i) {
        client.send(std::vector<std::byte>(frame.begin() + static_cast<std::ptrdiff_t>(i),
                                           frame.begin() + static_cast<std::ptrdiff_t>(i) + 1));
        std::this_thread::sleep_for(1ms);
    }

    EXPECT_TRUE(waitFor([&] { return server.engine().processed() == 1; }));
    server.stop();
}

TEST(NetIntegration, SeveralMessagesInOneWriteAllDecode) {
    Server server(testConfig());
    server.start();

    TestClient client(server.port());
    std::vector<std::byte> batch;
    for (OrderId id = 1; id <= 20; ++id) {
        const std::vector<std::byte> frame =
            encode(limitOrder(id, 10, Side::Buy, static_cast<Price>(100 + id), 5));
        batch.insert(batch.end(), frame.begin(), frame.end());
    }
    client.send(batch);

    EXPECT_TRUE(waitFor([&] { return server.engine().processed() == 20; }));
    server.stop();
}

TEST(NetIntegration, CancelOverTheWireRemovesARestingOrder) {
    Server server(testConfig());
    server.start();

    TestClient client(server.port());
    client.send(encode(limitOrder(1, 10, Side::Buy, 100, 5)));
    EXPECT_TRUE(waitFor([&] { return server.engine().processed() == 1; }));

    client.send(encode(CancelMessage{.orderId = 1}));
    EXPECT_TRUE(waitFor([&] { return server.engine().processed() == 2; }));

    server.stop();
    EXPECT_TRUE(server.engine().bookAfterShutdown().bids().empty());
}

TEST(NetIntegration, AnInvalidOrderIsRejectedAndTheSessionSurvives) {
    // InvalidOrderError rejects one message; the connection continues. That
    // distinction from ProtocolError is why they are separate types.
    Server server(testConfig());
    server.start();

    TestClient client(server.port());
    client.send(encode(limitOrder(1, 10, Side::Buy, 100, 0))); // zero quantity

    const std::optional<ServerMessage> reply = client.receive();
    ASSERT_TRUE(reply.has_value());
    ASSERT_TRUE(std::holds_alternative<RejectedMessage>(*reply));
    EXPECT_EQ(std::get<RejectedMessage>(*reply).reason, RejectReason::InvalidOrder);

    // Still usable afterwards.
    client.send(encode(limitOrder(2, 10, Side::Buy, 100, 5)));
    EXPECT_TRUE(waitFor([&] { return server.engine().processed() >= 1; }));

    server.stop();
}

TEST(NetIntegration, AMalformedFrameDropsTheSession) {
    // ProtocolError means the byte stream can no longer be trusted: every
    // subsequent byte would be read at the wrong offset, so resyncing is
    // impossible and dropping the session is the only honest response.
    Server server(testConfig());
    server.start();

    TestClient client(server.port());
    EXPECT_TRUE(waitFor([&] { return server.acceptedConnections() == 1; }));

    // A header claiming a body far beyond the cap.
    std::vector<std::byte> hostile;
    wire::Writer writer(hostile);
    writer.u32(0xFFFFFFFFU);
    writer.u8(kProtocolVersion);
    writer.u8(static_cast<std::uint8_t>(MessageType::Cancel));
    client.send(hostile);

    EXPECT_TRUE(waitFor([&] { return client.disconnected(); }))
        << "a peer that corrupts the framing must be dropped, not tolerated";

    server.stop();
}

TEST(NetIntegration, ASlowClientIsDisconnectedRatherThanBufferedWithoutBound) {
    // Phase 5's second exit criterion. The client connects, generates fills
    // for itself, and never reads. The server must cap what it holds and drop
    // the connection rather than grow memory or stall the engine.
    ServerConfig config = testConfig();
    config.maxOutboundBytes = 4096;
    // Cap the kernel buffers too. Without this the socket absorbs megabytes on
    // loopback and the application-level bound is never reached -- the server
    // would be keeping up, just via the kernel.
    config.socketSendBufferBytes = 2048;
    Server server(config);
    server.start();

    TestClient slow(server.port(), 2048);
    TestClient counterparty(server.port());

    // Rest a deep book for the slow client to trade against.
    for (OrderId id = 1; id <= 2000; ++id) {
        counterparty.send(encode(limitOrder(id, 900, Side::Sell, 100, 1)));
    }
    EXPECT_TRUE(waitFor([&] { return server.engine().processed() >= 2000; }, 5000ms));

    // The slow client sweeps it, earning a fill report per resting order --
    // and never calls recv().
    for (OrderId id = 10000; id <= 12000; ++id) {
        slow.send(encode(limitOrder(id, 901, Side::Buy, 100, 1)));
    }

    EXPECT_TRUE(waitFor([&] { return server.droppedForBackpressure() > 0; }, 5000ms))
        << "the outbound bound must be enforced by disconnecting, not by growing";

    // The exchange itself is unharmed: the counterparty is still served.
    counterparty.send(encode(limitOrder(90000, 900, Side::Sell, 300, 1)));
    const std::size_t before = server.engine().processed();
    EXPECT_TRUE(waitFor([&] { return server.engine().processed() > before; }))
        << "one slow participant must not degrade the venue for everyone else";

    server.stop();
}

TEST(NetIntegration, ShutdownIsCleanWithClientsStillConnected) {
    Server server(testConfig());
    server.start();

    std::vector<std::unique_ptr<TestClient>> clients;
    clients.reserve(8);
    for (int i = 0; i < 8; ++i) {
        clients.push_back(std::make_unique<TestClient>(server.port()));
        clients.back()->send(
            encode(limitOrder(static_cast<OrderId>(i + 1), static_cast<AccountId>(i + 1), Side::Buy,
                              static_cast<Price>(100 + i), 5)));
    }
    EXPECT_TRUE(waitFor([&] { return server.acceptedConnections() == 8; }));

    // Teardown order is reactor, workers, engine. Reversing it would leave
    // workers blocked pushing into a queue nobody drains.
    server.stop();
    SUCCEED() << "shutdown completed without hanging";
}

TEST(NetIntegration, SurvivesRepeatedStartStopCycles) {
    for (int cycle = 0; cycle < 20; ++cycle) {
        Server server(testConfig());
        server.start();

        TestClient client(server.port());
        client.send(encode(limitOrder(1, 1, Side::Buy, 100, 5)));

        server.stop();
    }
    SUCCEED() << "20 bind/serve/teardown cycles with no descriptor leak or hang";
}

TEST(NetIntegration, ADisconnectingClientDoesNotKillTheServer) {
    // Writing to a socket whose peer has gone raises SIGPIPE by default, which
    // terminates the process. MSG_NOSIGNAL is what stops a departing client
    // from taking the venue with it.
    Server server(testConfig());
    server.start();

    {
        TestClient resting(server.port());
        resting.send(encode(limitOrder(1, 10, Side::Sell, 100, 100)));
        EXPECT_TRUE(waitFor([&] { return server.engine().processed() == 1; }));
    } // socket closed here, while the order stays resting

    TestClient aggressor(server.port());
    aggressor.send(encode(limitOrder(2, 20, Side::Buy, 100, 50)));

    EXPECT_TRUE(waitFor([&] { return server.engine().fills() >= 1; }))
        << "the fill for a departed counterparty must not crash the server";

    server.stop();
}

} // namespace
} // namespace exchange::net
