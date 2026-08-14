// ThreadSafeQueue: bounding, blocking, and the shutdown path.
//
// The shutdown tests are the point of this file. A queue that works under load
// but hangs on close is a queue that hangs in production, and the failure is
// invisible until the process refuses to exit.

#include "concurrent/thread_safe_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <latch>
#include <memory>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

namespace exchange {
namespace {

using namespace std::chrono_literals;

TEST(ThreadSafeQueue, PushThenPopReturnsTheValue) {
    ThreadSafeQueue<int> queue(4);

    EXPECT_TRUE(queue.push(42));
    EXPECT_EQ(queue.size(), 1u);

    const std::optional<int> value = queue.pop();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 42);
    EXPECT_TRUE(queue.empty());
}

TEST(ThreadSafeQueue, PreservesFifoOrder) {
    ThreadSafeQueue<int> queue(8);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(queue.push(i));
    }

    std::vector<int> received;
    for (int i = 0; i < 5; ++i) {
        received.push_back(*queue.tryPop());
    }

    EXPECT_EQ(received, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(ThreadSafeQueue, CarriesMoveOnlyValues) {
    // The reason the queue never requires copyability: a SubmitCommand owns
    // its order through a unique_ptr.
    ThreadSafeQueue<std::unique_ptr<int>> queue(2);

    EXPECT_TRUE(queue.push(std::make_unique<int>(7)));

    std::optional<std::unique_ptr<int>> value = queue.pop();
    ASSERT_TRUE(value.has_value());
    ASSERT_NE(*value, nullptr);
    EXPECT_EQ(**value, 7);
}

TEST(ThreadSafeQueue, TryPushRefusesWhenFull) {
    ThreadSafeQueue<int> queue(2);

    EXPECT_TRUE(queue.tryPush(1));
    EXPECT_TRUE(queue.tryPush(2));
    EXPECT_FALSE(queue.tryPush(3)) << "the bound is what applies backpressure";
    EXPECT_EQ(queue.size(), 2u);
}

TEST(ThreadSafeQueue, TryPopRefusesWhenEmpty) {
    ThreadSafeQueue<int> queue(2);

    EXPECT_FALSE(queue.tryPop().has_value());
}

TEST(ThreadSafeQueue, CapacityIsNeverZero) {
    // A capacity of zero would make every push block forever with no consumer
    // able to relieve it.
    const ThreadSafeQueue<int> queue(0);

    EXPECT_EQ(queue.capacity(), 1u);
}

// ---------------------------------------------------------------------------
// Blocking and backpressure
// ---------------------------------------------------------------------------

TEST(ThreadSafeQueue, PushBlocksWhileFullAndResumesWhenDrained) {
    ThreadSafeQueue<int> queue(1);
    ASSERT_TRUE(queue.push(1));

    std::atomic<bool> secondPushReturned{false};
    std::thread producer([&] {
        EXPECT_TRUE(queue.push(2));
        secondPushReturned.store(true, std::memory_order_release);
    });

    // The producer must still be blocked: nothing has been consumed.
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(secondPushReturned.load(std::memory_order_acquire))
        << "a full queue must block its producer rather than growing";

    EXPECT_EQ(*queue.pop(), 1);
    producer.join();

    EXPECT_TRUE(secondPushReturned.load(std::memory_order_acquire));
    EXPECT_EQ(*queue.pop(), 2);
}

TEST(ThreadSafeQueue, PopBlocksUntilAValueArrives) {
    ThreadSafeQueue<int> queue(4);
    std::latch consumerReady(1);
    std::optional<int> received;

    std::thread consumer([&] {
        consumerReady.count_down();
        received = queue.pop();
    });

    consumerReady.wait();
    std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(queue.push(99));
    consumer.join();

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, 99);
}

// ---------------------------------------------------------------------------
// Shutdown -- where the lost wakeups live
// ---------------------------------------------------------------------------

TEST(ThreadSafeQueueShutdown, CloseWakesABlockedConsumer) {
    ThreadSafeQueue<int> queue(4);
    std::latch ready(1);
    std::optional<int> received{0};

    std::thread consumer([&] {
        ready.count_down();
        received = queue.pop();
    });

    ready.wait();
    std::this_thread::sleep_for(10ms);
    queue.close();
    consumer.join(); // hangs forever if the flag is not inside the predicate

    EXPECT_FALSE(received.has_value()) << "a closed, drained queue returns nullopt";
}

TEST(ThreadSafeQueueShutdown, CloseWakesABlockedProducer) {
    // The half people forget. Notifying only the consumer side leaves
    // producers blocked on not_full forever, and the process never exits.
    ThreadSafeQueue<int> queue(1);
    ASSERT_TRUE(queue.push(1));

    std::latch ready(1);
    std::atomic<bool> accepted{true};

    std::thread producer([&] {
        ready.count_down();
        accepted.store(queue.push(2), std::memory_order_release);
    });

    ready.wait();
    std::this_thread::sleep_for(10ms);
    queue.close();
    producer.join();

    EXPECT_FALSE(accepted.load(std::memory_order_acquire))
        << "a push released by close() must report failure, not silently drop";
}

TEST(ThreadSafeQueueShutdown, CloseWakesEveryWaiterNotJustOne) {
    // notify_all, not notify_one: waking one waiter leaves the rest asleep.
    constexpr std::size_t kConsumers = 8;
    ThreadSafeQueue<int> queue(4);
    std::latch ready(kConsumers);
    std::atomic<std::size_t> woken{0};
    std::vector<std::thread> consumers;

    consumers.reserve(kConsumers);
    for (std::size_t i = 0; i < kConsumers; ++i) {
        consumers.emplace_back([&] {
            ready.count_down();
            (void)queue.pop();
            woken.fetch_add(1, std::memory_order_relaxed);
        });
    }

    ready.wait();
    std::this_thread::sleep_for(10ms);
    queue.close();
    for (std::thread& consumer : consumers) {
        consumer.join();
    }

    EXPECT_EQ(woken.load(std::memory_order_relaxed), kConsumers);
}

TEST(ThreadSafeQueueShutdown, ClosingDoesNotDiscardQueuedWork) {
    // close() refuses new items; it does not throw away accepted ones. This is
    // what lets the engine finish work it has already taken responsibility for.
    ThreadSafeQueue<int> queue(8);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(queue.push(i));
    }

    queue.close();

    std::vector<int> drained;
    while (const std::optional<int> value = queue.pop()) {
        drained.push_back(*value);
    }

    EXPECT_EQ(drained, (std::vector<int>{0, 1, 2, 3}));
    EXPECT_FALSE(queue.pop().has_value()) << "and only then does it report exhaustion";
}

TEST(ThreadSafeQueueShutdown, PushAfterCloseIsRefused) {
    ThreadSafeQueue<int> queue(4);
    queue.close();

    EXPECT_FALSE(queue.push(1));
    EXPECT_FALSE(queue.tryPush(1));
    EXPECT_TRUE(queue.empty());
}

TEST(ThreadSafeQueueShutdown, CloseIsIdempotent) {
    ThreadSafeQueue<int> queue(4);

    queue.close();
    queue.close();
    queue.close();

    EXPECT_TRUE(queue.closed());
}

// ---------------------------------------------------------------------------
// Under load
// ---------------------------------------------------------------------------

TEST(ThreadSafeQueue, LosesNothingUnderManyProducers) {
    // The invariant that matters: every value pushed is popped exactly once.
    // Run under ThreadSanitizer this is also the file's main race check.
    constexpr std::size_t kProducers = 8;
    constexpr int kPerProducer = 500;

    ThreadSafeQueue<int> queue(16); // deliberately small, so producers block
    std::atomic<long long> consumedSum{0};
    std::atomic<std::size_t> consumedCount{0};

    std::thread consumer([&] {
        while (const std::optional<int> value = queue.pop()) {
            consumedSum.fetch_add(*value, std::memory_order_relaxed);
            consumedCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 1; i <= kPerProducer; ++i) {
                EXPECT_TRUE(queue.push(i));
            }
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }

    queue.close();
    consumer.join();

    constexpr long long kExpectedSum =
        static_cast<long long>(kProducers) * kPerProducer * (kPerProducer + 1) / 2;
    EXPECT_EQ(consumedCount.load(), kProducers * kPerProducer);
    EXPECT_EQ(consumedSum.load(), kExpectedSum);
}

TEST(ThreadSafeQueue, NeverExceedsItsBound) {
    constexpr std::size_t kCapacity = 4;
    ThreadSafeQueue<int> queue(kCapacity);
    std::atomic<std::size_t> highWater{0};
    std::atomic<bool> stop{false};

    std::thread observer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const std::size_t observed = queue.size();
            std::size_t previous = highWater.load(std::memory_order_relaxed);
            while (observed > previous && !highWater.compare_exchange_weak(
                                              previous, observed, std::memory_order_relaxed)) {
            }
        }
    });

    std::thread producer([&] {
        for (int i = 0; i < 2000; ++i) {
            EXPECT_TRUE(queue.push(i));
        }
    });

    std::size_t consumed = 0;
    while (consumed < 2000) {
        if (queue.pop().has_value()) {
            ++consumed;
        }
    }

    producer.join();
    stop.store(true, std::memory_order_release);
    observer.join();

    EXPECT_LE(highWater.load(), kCapacity) << "an unbounded queue is a memory leak with a schedule";
}

} // namespace
} // namespace exchange
