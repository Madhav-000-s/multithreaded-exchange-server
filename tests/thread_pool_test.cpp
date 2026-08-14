// ThreadPool and the move-only Task it carries.

#include "concurrent/task.hpp"
#include "concurrent/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace exchange {
namespace {

TEST(Task, CarriesAMoveOnlyCallable) {
    // std::function would reject this outright: it demands a copyable target
    // for the sake of a copy constructor nothing here ever calls.
    auto owned = std::make_unique<int>(5);
    int observed = 0;

    Task task([held = std::move(owned), &observed] { observed = *held; });
    task();

    EXPECT_EQ(observed, 5);
    static_assert(!std::is_copy_constructible_v<Task>);
}

TEST(Task, DefaultConstructedIsEmpty) {
    const Task task;

    EXPECT_FALSE(static_cast<bool>(task));
}

TEST(Task, MovingTransfersTheCallable) {
    int calls = 0;
    Task original([&calls] { ++calls; });

    Task moved = std::move(original);
    moved();

    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(static_cast<bool>(moved));
}

// ---------------------------------------------------------------------------

TEST(ThreadPool, RunsEverySubmittedTask) {
    constexpr std::size_t kTasks = 500;
    std::atomic<std::size_t> ran{0};

    {
        ThreadPool pool(4, 32);
        for (std::size_t i = 0; i < kTasks; ++i) {
            EXPECT_TRUE(pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
        }
        pool.stop(); // closes, drains, joins
        EXPECT_EQ(pool.completed(), kTasks);
    }

    EXPECT_EQ(ran.load(), kTasks);
}

TEST(ThreadPool, DrainsAcceptedWorkBeforeExiting) {
    // stop() must not discard work already accepted; a task queued a
    // microsecond before shutdown has still been promised to the caller.
    constexpr std::size_t kTasks = 200;
    std::atomic<std::size_t> ran{0};

    ThreadPool pool(2, kTasks);
    for (std::size_t i = 0; i < kTasks; ++i) {
        EXPECT_TRUE(pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
    }
    pool.stop();

    EXPECT_EQ(ran.load(), kTasks);
}

TEST(ThreadPool, RefusesWorkOnceStopped) {
    ThreadPool pool(2, 8);
    pool.stop();

    EXPECT_FALSE(pool.submit([] {}));
    EXPECT_FALSE(pool.trySubmit([] {}));
}

TEST(ThreadPool, SurvivesATaskThatThrows) {
    // An exception escaping a thread entry point is std::terminate. A worker
    // that died on each bad message would leave no pool at all by the tenth.
    std::atomic<std::size_t> ran{0};

    ThreadPool pool(2, 16);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(pool.submit([] { throw std::runtime_error("bad task"); }));
    }
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
    }
    pool.stop();

    EXPECT_EQ(pool.failed(), 10u) << "failures are counted, not silently swallowed";
    EXPECT_EQ(ran.load(), 10u) << "and the pool kept working afterwards";
}

TEST(ThreadPool, StopIsIdempotentAndSafeFromAnyThread) {
    ThreadPool pool(4, 16);
    std::latch ready(3);
    std::vector<std::thread> stoppers;

    stoppers.reserve(3);
    for (int i = 0; i < 3; ++i) {
        stoppers.emplace_back([&] {
            ready.arrive_and_wait();
            pool.stop(); // only one may reach join(); joining twice is UB
        });
    }
    for (std::thread& stopper : stoppers) {
        stopper.join();
    }

    EXPECT_FALSE(pool.submit([] {}));
}

TEST(ThreadPool, DestructorJoinsWithoutAnExplicitStop) {
    // A joinable std::thread destroyed calls std::terminate, so the
    // destructor cannot leave this to the caller.
    std::atomic<std::size_t> ran{0};
    {
        ThreadPool pool(3, 16);
        for (int i = 0; i < 50; ++i) {
            EXPECT_TRUE(pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
        }
    }

    EXPECT_EQ(ran.load(), 50u);
}

TEST(ThreadPool, AppliesBackpressureRatherThanGrowing) {
    ThreadPool pool(1, 2);
    std::atomic<bool> release{false};
    std::atomic<std::size_t> started{0};

    EXPECT_TRUE(pool.submit([&] {
        started.fetch_add(1, std::memory_order_relaxed);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }));

    // Wait until the worker has actually taken the task. Without this the
    // queue may still hold it, one slot is already gone, and the assertions
    // below race the scheduler -- a test that fails perhaps one run in ten.
    while (started.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }

    // The single worker is now occupied, so the queue fills and then refuses.
    EXPECT_TRUE(pool.trySubmit([] {}));
    EXPECT_TRUE(pool.trySubmit([] {}));
    EXPECT_FALSE(pool.trySubmit([] {})) << "the bound is the whole point";

    release.store(true, std::memory_order_release);
    pool.stop();
}

TEST(ThreadPool, ClampsThreadCountToAtLeastOne) {
    const ThreadPool pool(0, 4);

    EXPECT_EQ(pool.threadCount(), 1u);
}

} // namespace
} // namespace exchange
