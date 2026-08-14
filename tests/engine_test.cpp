// The engine thread, driven concurrently. Also the shutdown-cycle loop that
// PLAN.md makes an exit criterion: a lost wakeup shows up as a hang, and a
// hang only shows up if you run the sequence enough times to hit the window.

#include "analytics/l2_feed.hpp"
#include "concurrent/thread_pool.hpp"
#include "core/command.hpp"
#include "core/engine.hpp"
#include "core/types.hpp"

#include "order_factory.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <latch>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace exchange {
namespace {

using test::kAlice;
using test::kBob;
using test::kCarol;

[[nodiscard]] Command submitOf(OrderId id, Side side, Price price, Quantity qty,
                               AccountId account) {
    return SubmitCommand{.order = test::limit(id, side, price, qty, account)};
}

TEST(Engine, AppliesCommandsAndReachesTheBook) {
    Engine engine(64);
    engine.start();

    EXPECT_TRUE(engine.submit(submitOf(1, Side::Buy, 100, 10, kAlice)));
    EXPECT_TRUE(engine.submit(submitOf(2, Side::Sell, 110, 10, kBob)));
    engine.stop();

    const Book& book = engine.bookAfterShutdown();
    EXPECT_EQ(*book.bestBid(), 100);
    EXPECT_EQ(*book.bestAsk(), 110);
    EXPECT_EQ(engine.processed(), 2u);
}

TEST(Engine, MatchesAndReportsFills) {
    Engine engine(64);
    std::vector<Fill> observed;
    // The handler runs on the engine thread, and only the engine thread, so
    // this needs no lock -- but it is read after join(), which is what makes
    // that safe rather than merely likely.
    engine.onFill([&observed](const Fill& fill) { observed.push_back(fill); });
    engine.start();

    EXPECT_TRUE(engine.submit(submitOf(1, Side::Sell, 100, 10, kAlice)));
    EXPECT_TRUE(engine.submit(submitOf(2, Side::Buy, 100, 4, kBob)));
    engine.stop();

    ASSERT_EQ(observed.size(), 1u);
    EXPECT_EQ(observed[0].quantity, 4u);
    EXPECT_EQ(observed[0].price, 100);
    EXPECT_EQ(engine.fills(), 1u);
}

TEST(Engine, HandlesCancelAndModify) {
    Engine engine(64);
    engine.start();

    EXPECT_TRUE(engine.submit(submitOf(1, Side::Buy, 100, 10, kAlice)));
    EXPECT_TRUE(engine.submit(submitOf(2, Side::Buy, 99, 10, kBob)));
    EXPECT_TRUE(engine.submit(ModifyCommand{.id = 1, .quantity = 4, .price = std::nullopt}));
    EXPECT_TRUE(engine.submit(CancelCommand{.id = 2}));
    engine.stop();

    const Book& book = engine.bookAfterShutdown();
    EXPECT_EQ(book.bids().qtyAt(100), 4u);
    EXPECT_EQ(book.bids().qtyAt(99), 0u);
}

TEST(Engine, CountsRejectionsWithoutDyingOnThem) {
    // A malformed order must not take down the only thread that owns the book.
    Engine engine(64);
    engine.start();

    EXPECT_TRUE(engine.submit(submitOf(1, Side::Buy, 100, 10, kAlice)));
    EXPECT_TRUE(engine.submit(submitOf(1, Side::Buy, 99, 5, kBob))); // duplicate id
    EXPECT_TRUE(engine.submit(SubmitCommand{.order = test::limit(2, Side::Buy, 98, 0, kBob)}));
    EXPECT_TRUE(engine.submit(CancelCommand{.id = 999})); // unknown
    EXPECT_TRUE(engine.submit(submitOf(3, Side::Buy, 97, 7, kCarol)));
    engine.stop();

    EXPECT_EQ(engine.rejected(), 3u);
    const Book& book = engine.bookAfterShutdown();
    EXPECT_EQ(book.bids().qtyAt(100), 10u) << "the valid orders still landed";
    EXPECT_EQ(book.bids().qtyAt(97), 7u);
}

TEST(Engine, SerialisesConcurrentProducers) {
    // Many threads submit; one thread matches. The book is never locked
    // because exactly one thread ever touches it.
    constexpr std::size_t kProducers = 8;
    constexpr std::size_t kPerProducer = 200;

    Engine engine(32); // small, so producers genuinely block
    engine.start();

    std::latch start(kProducers);
    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                const OrderId id = p * kPerProducer + i + 1;
                // All the same account, so nothing crosses and everything rests.
                EXPECT_TRUE(engine.submit(
                    submitOf(id, Side::Buy, static_cast<Price>(50 + (id % 20)), 1, kAlice)));
            }
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    engine.stop();

    EXPECT_EQ(engine.processed(), kProducers * kPerProducer);
    EXPECT_EQ(engine.bookAfterShutdown().bids().orderCount(), kProducers * kPerProducer);
}

TEST(Engine, DrainsAcceptedCommandsOnShutdown) {
    Engine engine(1024);
    engine.start();

    for (OrderId id = 1; id <= 500; ++id) {
        EXPECT_TRUE(engine.submit(submitOf(id, Side::Buy, 100, 1, kAlice)));
    }
    engine.stop();

    EXPECT_EQ(engine.processed(), 500u) << "close() refuses new work, it does not discard queued";
    EXPECT_EQ(engine.bookAfterShutdown().bids().qtyAt(100), 500u);
}

TEST(Engine, RefusesCommandsOnceStopped) {
    Engine engine(16);
    engine.start();
    engine.stop();

    EXPECT_FALSE(engine.submit(submitOf(1, Side::Buy, 100, 10, kAlice)));
    EXPECT_FALSE(engine.running());
}

TEST(Engine, StopIsIdempotentFromAnyThread) {
    Engine engine(16);
    engine.start();

    std::latch ready(4);
    std::vector<std::thread> stoppers;
    stoppers.reserve(4);
    for (int i = 0; i < 4; ++i) {
        stoppers.emplace_back([&] {
            ready.arrive_and_wait();
            engine.stop();
        });
    }
    for (std::thread& stopper : stoppers) {
        stopper.join();
    }

    EXPECT_FALSE(engine.running());
}

TEST(Engine, DestructorStopsWithoutAnExplicitCall) {
    std::atomic<std::size_t> fills{0};
    {
        Engine engine(64);
        engine.onFill([&fills](const Fill&) { fills.fetch_add(1, std::memory_order_relaxed); });
        engine.start();
        EXPECT_TRUE(engine.submit(submitOf(1, Side::Sell, 100, 10, kAlice)));
        EXPECT_TRUE(engine.submit(submitOf(2, Side::Buy, 100, 10, kBob)));
    }

    EXPECT_EQ(fills.load(), 1u);
}

// ---------------------------------------------------------------------------
// The shutdown-cycle exit criterion
// ---------------------------------------------------------------------------

TEST(EngineShutdown, SurvivesRepeatedStartStopCycles) {
    // PLAN.md makes this an exit criterion, and it is the right shape of test:
    // a lost wakeup is a narrow scheduling window, so one clean shutdown proves
    // nothing. Each cycle races producers against the close.
    constexpr int kCycles = 100;

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        Engine engine(8);
        engine.start();

        std::vector<std::thread> producers;
        producers.reserve(3);
        for (int p = 0; p < 3; ++p) {
            producers.emplace_back([&engine, p] {
                for (int i = 0; i < 40; ++i) {
                    const OrderId id = static_cast<OrderId>(p * 40 + i + 1);
                    // Ignored deliberately: a push racing the close legitimately
                    // returns false. Blocking forever is the bug being hunted.
                    (void)engine.submit(
                        SubmitCommand{.order = test::limit(id, Side::Buy, 100, 1, kAlice)});
                }
            });
        }

        engine.stop();
        for (std::thread& producer : producers) {
            producer.join();
        }
    }

    SUCCEED() << kCycles << " start/stop cycles completed without hanging";
}

TEST(EngineShutdown, ProducersBlockedOnAFullQueueAreReleased) {
    // The specific deadlock: producers waiting on not_full while the engine
    // stops. If close() notified only the consumer condvar, this hangs.
    Engine engine(1);
    engine.start();

    std::latch ready(4);
    std::atomic<std::size_t> returned{0};
    std::vector<std::thread> producers;

    producers.reserve(4);
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&, p] {
            ready.arrive_and_wait();
            for (int i = 0; i < 100; ++i) {
                const OrderId id = static_cast<OrderId>(p * 100 + i + 1);
                (void)engine.submit(
                    SubmitCommand{.order = test::limit(id, Side::Buy, 100, 1, kAlice)});
            }
            returned.fetch_add(1, std::memory_order_relaxed);
        });
    }

    ready.wait();
    engine.stop();
    for (std::thread& producer : producers) {
        producer.join();
    }

    EXPECT_EQ(returned.load(), 4u) << "every producer returned rather than blocking forever";
}

// ---------------------------------------------------------------------------
// L2 feed
// ---------------------------------------------------------------------------

TEST(L2Publisher, PublishesBothSidesBestFirst) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Buy, 99, 20, kBob));
    book.submit(test::limit(3, Side::Sell, 110, 5, kCarol));

    analytics::L2Publisher publisher(5);
    publisher.publish(book);

    const analytics::L2Snapshot snapshot = publisher.read();
    ASSERT_EQ(snapshot.bids.size(), 2u);
    EXPECT_EQ(snapshot.bids[0].price, 100);
    EXPECT_EQ(snapshot.bids[1].price, 99);
    ASSERT_EQ(snapshot.asks.size(), 1u);
    EXPECT_EQ(snapshot.asks[0].price, 110);
    EXPECT_EQ(snapshot.sequence, 1u);
}

TEST(L2Publisher, SequenceAdvancesOnEveryPublication) {
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));

    analytics::L2Publisher publisher;
    publisher.publish(book);
    publisher.publish(book);
    publisher.publish(book);

    EXPECT_EQ(publisher.sequence(), 3u) << "a reader can tell how many updates it missed";
}

TEST(L2Publisher, ServesManyReadersWhileOneWriterPublishes) {
    // The access pattern that justifies a shared_mutex at all: one writer,
    // many concurrent readers. Under TSan this is the race check.
    Book book;
    book.submit(test::limit(1, Side::Buy, 100, 10, kAlice));
    book.submit(test::limit(2, Side::Sell, 110, 10, kBob));

    analytics::L2Publisher publisher;
    publisher.publish(book);

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> reads{0};
    std::vector<std::thread> readers;

    readers.reserve(6);
    for (int i = 0; i < 6; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                const analytics::L2Snapshot snapshot = publisher.read();
                // A snapshot is never torn: both sides come from one book and
                // are swapped in under a single unique lock.
                if (!snapshot.bids.empty()) {
                    EXPECT_EQ(snapshot.bids[0].price, 100);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int i = 0; i < 200; ++i) {
        publisher.publish(book);
    }
    stop.store(true, std::memory_order_release);
    for (std::thread& reader : readers) {
        reader.join();
    }

    EXPECT_GT(reads.load(), 0u);
    EXPECT_GE(publisher.sequence(), 201u);
}

} // namespace
} // namespace exchange
