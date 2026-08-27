// Recovery, including the kill -9 harness that is Phase 6's exit criterion.

#include "core/apply.hpp"
#include "core/book.hpp"
#include "core/command.hpp"
#include "core/engine.hpp"
#include "core/types.hpp"
#include "store/recovery.hpp"
#include "store/sqlite_store.hpp"
#include "store/write_ahead_log.hpp"

#include "book_snapshot.hpp"
#include "order_factory.hpp"
#include "temp_path.hpp"

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace exchange::store {
namespace {

using namespace std::chrono_literals;
using test::BookSnapshot;
using test::kAlice;
using test::kBob;
using test::snapshot;
using test::TempPath;

[[nodiscard]] Command submitOf(OrderId id, Side side, Price price, Quantity qty,
                               AccountId account) {
    return SubmitCommand{.order = test::limit(id, side, price, qty, account)};
}

// ---------------------------------------------------------------------------
// Replay through the live path
// ---------------------------------------------------------------------------

TEST(Recovery, RebuildsABookIdenticalToTheOneThatWasLogged) {
    const TempPath wal("recover-identical");

    BookSnapshot before;
    {
        WriteAheadLog log(WalConfig{.path = wal.str(), .policy = SyncPolicy::Always});
        Book book;

        // A mixture that exercises resting, matching and amendment, so the
        // comparison is not trivially satisfied by an empty book.
        const auto drive = [&](Command command) {
            (void)log.append(command);
            (void)applyCommand(book, std::move(command));
        };

        drive(submitOf(1, Side::Buy, 100, 10, kAlice));
        drive(submitOf(2, Side::Buy, 99, 20, kAlice));
        drive(submitOf(3, Side::Sell, 105, 15, kBob));
        drive(submitOf(4, Side::Sell, 100, 4, kBob)); // crosses order 1
        drive(ModifyCommand{.id = 2, .quantity = 5, .price = std::nullopt});
        drive(CancelCommand{.id = 3});

        before = snapshot(book);
    }

    Book recovered;
    const RecoveryReport report = recoverBookOnly(wal.str(), recovered);

    EXPECT_EQ(report.recordsReplayed, 6u);
    EXPECT_EQ(report.recordsRejected, 0u);
    EXPECT_FALSE(report.logTruncated);

    // Field for field, including sequence numbers -- so a replay that restored
    // the right orders in the wrong queue positions would still fail.
    EXPECT_EQ(before, snapshot(recovered));
}

TEST(Recovery, ReproducesFillsRatherThanOnlyRestingOrders) {
    const TempPath wal("recover-fills");

    {
        WriteAheadLog log(WalConfig{.path = wal.str(), .policy = SyncPolicy::Always});
        Book book;

        // A lambda rather than a loop over an initializer_list: Command owns
        // its order through a unique_ptr, so it is move-only, and an
        // initializer_list only ever hands out const references.
        const auto drive = [&](Command command) {
            (void)log.append(command);
            (void)applyCommand(book, std::move(command));
        };
        drive(submitOf(1, Side::Sell, 100, 10, kAlice));
        drive(submitOf(2, Side::Buy, 100, 6, kBob));
    }

    Book recovered;
    const RecoveryReport report = recoverBookOnly(wal.str(), recovered);

    EXPECT_EQ(report.fillsReplayed, 1u);
    EXPECT_EQ(recovered.asks().qtyAt(100), 4u) << "the partially filled remainder is restored";
}

TEST(Recovery, AnEmptyOrAbsentLogRecoversAnEmptyBook) {
    const TempPath wal("recover-empty");

    Book recovered;
    const RecoveryReport report = recoverBookOnly(wal.str(), recovered);

    EXPECT_EQ(report.recordsReplayed, 0u);
    EXPECT_TRUE(recovered.bids().empty());
    EXPECT_TRUE(recovered.asks().empty());
}

TEST(Recovery, CountsRefusalsWithoutAborting) {
    // The log records what was *asked*. A command the book declined at run
    // time is declined identically on replay -- counting it is right, and
    // aborting recovery over it would be wrong.
    const TempPath wal("recover-refusals");

    {
        WriteAheadLog log(WalConfig{.path = wal.str(), .policy = SyncPolicy::Always});
        (void)log.append(submitOf(1, Side::Buy, 100, 10, kAlice));
        (void)log.append(CancelCommand{.id = 999}); // never existed
    }

    Book recovered;
    const RecoveryReport report = recoverBookOnly(wal.str(), recovered);

    EXPECT_EQ(report.recordsReplayed, 1u);
    EXPECT_EQ(report.recordsRejected, 1u);
    EXPECT_EQ(recovered.bids().qtyAt(100), 10u) << "the valid record still applied";
}

TEST(Recovery, TrimsADamagedTailAndKeepsTheRest) {
    const TempPath wal("recover-trim");

    {
        WriteAheadLog log(WalConfig{.path = wal.str(), .policy = SyncPolicy::Always});
        for (OrderId id = 1; id <= 3; ++id) {
            (void)log.append(submitOf(id, Side::Buy, 100 + static_cast<Price>(id), 1, kAlice));
        }
    }

    // Append garbage, as a kill mid-write would leave.
    {
        std::FILE* file = std::fopen(wal.str().c_str(), "ab");
        ASSERT_NE(file, nullptr);
        const char junk[] = {'\x40', '\x00', '\x00', '\x00', '\x01', '\x02'};
        std::fwrite(junk, 1, sizeof(junk), file);
        std::fclose(file);
    }

    Book recovered;
    const RecoveryReport report = recoverBookOnly(wal.str(), recovered);

    EXPECT_EQ(report.recordsReplayed, 3u);
    EXPECT_TRUE(report.logTruncated);
    EXPECT_EQ(recovered.bids().orderCount(), 3u);

    // And the log is clean afterwards, so the next append starts from a
    // record boundary rather than after garbage.
    Book again;
    const RecoveryReport second = recoverBookOnly(wal.str(), again);
    EXPECT_FALSE(second.logTruncated);
    EXPECT_EQ(second.recordsReplayed, 3u);
}

TEST(Recovery, BringsSqliteForwardFromTheLog) {
    const TempPath wal("recover-db-wal");
    const TempPath db("recover-db-sqlite");

    {
        WriteAheadLog log(WalConfig{.path = wal.str(), .policy = SyncPolicy::Always});
        (void)log.append(submitOf(1, Side::Sell, 100, 10, kAlice));
        (void)log.append(submitOf(2, Side::Buy, 100, 10, kBob));
    }

    Book book;
    SqliteStore store(db.str());
    store.createAccount(kAlice, 100'000);
    store.createAccount(kBob, 100'000);

    const RecoveryReport report = recover(wal.str(), book, store);

    EXPECT_EQ(report.recordsReplayed, 2u);
    EXPECT_EQ(report.fillsReplayed, 1u);
    EXPECT_EQ(report.lastSequence, 2u);
    EXPECT_EQ(store.ledgerImbalance(), 0);
}

// ---------------------------------------------------------------------------
// The engine writes ahead
// ---------------------------------------------------------------------------

TEST(EngineDurability, LogsEveryCommandItApplies) {
    const TempPath wal("engine-logs");

    {
        WriteAheadLog log(WalConfig{.path = wal.str(), .policy = SyncPolicy::Always});
        Engine engine(256);
        engine.setCommandLog(&log);
        engine.start();

        for (OrderId id = 1; id <= 20; ++id) {
            EXPECT_TRUE(engine.submit(submitOf(id, Side::Buy, 100, 1, kAlice)));
        }
        engine.stop();

        EXPECT_EQ(engine.processed(), 20u);
        EXPECT_EQ(engine.storageFailures(), 0u);
        EXPECT_EQ(log.recordsWritten(), 20u);
    }

    Book recovered;
    EXPECT_EQ(recoverBookOnly(wal.str(), recovered).recordsReplayed, 20u);
    EXPECT_EQ(recovered.bids().qtyAt(100), 20u);
}

TEST(EngineDurability, DoesNotApplyACommandItCouldNotLog) {
    // The write-ahead invariant, tested from the failure side: if the record
    // cannot be made durable, the book must not change. An unlogged mutation
    // is exactly the divergence the log exists to prevent, and it would
    // survive into recovery undetected.
    class FailingLog final : public ICommandLog {
    public:
        Sequence append(const Command&) override {
            ++attempts;
            throw StorageError("disk full");
        }

        void flush() override {}

        std::size_t attempts{0};
    };

    FailingLog log;
    Engine engine(64);
    engine.setCommandLog(&log);
    engine.start();

    EXPECT_TRUE(engine.submit(submitOf(1, Side::Buy, 100, 10, kAlice)));
    engine.stop();

    EXPECT_EQ(log.attempts, 1u);
    EXPECT_EQ(engine.storageFailures(), 1u);
    EXPECT_EQ(engine.processed(), 0u);
    EXPECT_TRUE(engine.bookAfterShutdown().bids().empty())
        << "the book must not contain an order the log does not";
}

// ---------------------------------------------------------------------------
// kill -9, the exit criterion
// ---------------------------------------------------------------------------

/// Runs the worker, kills it after roughly `killAfter` acknowledgements, and
/// returns every id it acknowledged before dying.
[[nodiscard]] std::set<OrderId> runAndKill(const std::string& workerPath,
                                           const std::string& walPath, const char* policy,
                                           std::size_t killAfter) {
    std::array<int, 2> pipeFds{};
    EXPECT_EQ(::pipe(pipeFds.data()), 0);

    const pid_t child = ::fork();
    EXPECT_GE(child, 0);

    if (child == 0) {
        // Child: stdout to the pipe, then exec. Nothing between fork and exec
        // allocates or takes a lock, which is the rule that keeps forking from
        // a threaded parent safe.
        ::dup2(pipeFds[1], STDOUT_FILENO);
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        ::execl(workerPath.c_str(), workerPath.c_str(), walPath.c_str(), policy, "100000", nullptr);
        ::_exit(127);
    }

    ::close(pipeFds[1]);

    std::set<OrderId> acknowledged;
    std::string pending;
    std::array<char, 4096> buffer{};

    while (acknowledged.size() < killAfter) {
        const ssize_t got = ::read(pipeFds[0], buffer.data(), buffer.size());
        if (got <= 0) {
            break;
        }
        pending.append(buffer.data(), static_cast<std::size_t>(got));

        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            acknowledged.insert(std::strtoull(pending.substr(0, newline).c_str(), nullptr, 10));
            pending.erase(0, newline + 1);
        }
    }

    // SIGKILL, not SIGTERM: the point is a process that gets no chance to
    // flush, close a file, or run a destructor. Anything the log does not
    // already hold is gone.
    ::kill(child, SIGKILL);

    int status = 0;
    ::waitpid(child, &status, 0);
    ::close(pipeFds[0]);

    EXPECT_TRUE(WIFSIGNALED(status)) << "the worker should have been killed, not exited";
    return acknowledged;
}

class CrashRecovery : public ::testing::Test {
protected:
    /// The worker sits next to the test binary, which CTest runs from the
    /// build tree.
    [[nodiscard]] static std::string workerPath() {
        const char* fromEnv = std::getenv("EXCHANGE_CRASH_WORKER");
        return fromEnv != nullptr ? std::string(fromEnv) : std::string("./crash_worker");
    }
};

TEST_F(CrashRecovery, EverythingAcknowledgedSurvivesTenConsecutiveKills) {
    // PLAN.md's exit criterion. Ten cycles, each killed at a different point
    // in the write cycle, because a durability bug is a narrow window and one
    // crash proves nothing.
    constexpr int kCycles = 10;

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        const TempPath wal("crash-cycle-" + std::to_string(cycle));

        // Varying the kill point moves it around the append/fsync/apply cycle
        // rather than always landing in the same place.
        const std::size_t killAfter = 20 + static_cast<std::size_t>(cycle) * 17;

        const std::set<OrderId> acknowledged =
            runAndKill(workerPath(), wal.str(), "always", killAfter);

        ASSERT_GE(acknowledged.size(), killAfter)
            << "cycle " << cycle << ": the worker did not get far enough to be a test";

        Book recovered;
        const RecoveryReport report = recoverBookOnly(wal.str(), recovered);

        // The claim: every id the worker acknowledged is present after
        // recovery. Ids beyond that may or may not survive -- they were never
        // promised -- so they are not asserted either way.
        std::set<OrderId> present;
        for (const Order& order : recovered.bids()) {
            present.insert(order.id());
        }
        for (const Order& order : recovered.asks()) {
            present.insert(order.id());
        }

        std::size_t missing = 0;
        for (const OrderId id : acknowledged) {
            // An acknowledged order is either still resting or was consumed by
            // a later trade, so presence in the book is not the test --
            // presence in the *log* is. The book is checked for consistency
            // separately.
            if (id > report.lastSequence) {
                ++missing;
            }
        }

        EXPECT_EQ(missing, 0u) << "cycle " << cycle << ": " << missing
                               << " acknowledged orders were not in the recovered log";
        EXPECT_GE(report.recordsReplayed + report.recordsRejected, acknowledged.size())
            << "cycle " << cycle << ": the log holds fewer records than were acknowledged";
        EXPECT_FALSE(recovered.isCrossed())
            << "cycle " << cycle << ": recovery produced a crossed book";
    }
}

TEST_F(CrashRecovery, ADamagedTailIsTrimmedAndTheLogIsUsableAgain) {
    // After a kill the log very often ends mid-record. Recovery must trim it
    // and leave a file the next run can append to.
    const TempPath wal("crash-reusable");

    const std::set<OrderId> acknowledged = runAndKill(workerPath(), wal.str(), "always", 50);
    ASSERT_GE(acknowledged.size(), 50u);

    Book first;
    const RecoveryReport report = recoverBookOnly(wal.str(), first);
    EXPECT_GT(report.recordsReplayed, 0u);

    // Appending after recovery must produce a log that still replays cleanly.
    {
        WriteAheadLog log(WalConfig{.path = wal.str(), .policy = SyncPolicy::Always});
        (void)log.append(submitOf(999'999, Side::Buy, 500, 1, kAlice));
    }

    Book second;
    const RecoveryReport after = recoverBookOnly(wal.str(), second);
    EXPECT_FALSE(after.logTruncated) << "the trimmed log accepted a clean append";
    EXPECT_EQ(after.recordsReplayed + after.recordsRejected,
              report.recordsReplayed + report.recordsRejected + 1);
}

TEST_F(CrashRecovery, RecoveryIsDeterministic) {
    // Replaying the same log twice must produce the same book. If it does not,
    // matching depends on something outside the log and recovery cannot be
    // trusted at all.
    const TempPath wal("crash-deterministic");

    const std::set<OrderId> acknowledged = runAndKill(workerPath(), wal.str(), "always", 80);
    ASSERT_GE(acknowledged.size(), 80u);

    Book first;
    (void)recoverBookOnly(wal.str(), first);
    const BookSnapshot firstPass = snapshot(first);

    Book second;
    (void)recoverBookOnly(wal.str(), second);

    EXPECT_EQ(firstPass, snapshot(second));
}

// ---------------------------------------------------------------------------
// The harness itself
// ---------------------------------------------------------------------------

TEST_F(CrashRecovery, TheDurabilityAssertionIsFalsifiable) {
    // A harness that cannot fail proves nothing. Here the log is deliberately
    // truncated below the acknowledged count, which is exactly the divergence
    // the crash tests assert never happens -- and the check must notice.
    const TempPath wal("crash-falsifiable");

    const std::set<OrderId> acknowledged = runAndKill(workerPath(), wal.str(), "always", 60);
    ASSERT_GE(acknowledged.size(), 60u);

    Book intact;
    const RecoveryReport before = recoverBookOnly(wal.str(), intact);
    ASSERT_GE(before.recordsReplayed + before.recordsRejected, acknowledged.size());

    // Cut the log roughly in half, simulating a durability guarantee that was
    // not honoured.
    WriteAheadLog::truncateTo(wal.str(), before.validBytes / 2);

    Book damaged;
    const RecoveryReport after = recoverBookOnly(wal.str(), damaged);

    EXPECT_LT(after.recordsReplayed + after.recordsRejected, acknowledged.size())
        << "the check would not have noticed a log that lost acknowledged records";
}

// ---------------------------------------------------------------------------
// The durability/throughput trade, measured
// ---------------------------------------------------------------------------

TEST(WalBenchmark, SyncPolicyCostsWhatItClaims) {
    // PLAN.md asks for this to be benchmarked rather than asserted, because it
    // is the central trade in the persistence design. Reported rather than
    // gated on a threshold: the absolute numbers depend on the device, and a
    // test that fails on a slow disk is a test that gets disabled.
    constexpr int kRecords = 2000;

    struct Measured {
        const char* name;
        SyncPolicy policy;
        double microsPerRecord;
        std::size_t syncs;
    };

    std::vector<Measured> results;

    for (const auto& [name, policy] :
         {std::pair{"always", SyncPolicy::Always}, std::pair{"batch", SyncPolicy::Batch},
          std::pair{"never", SyncPolicy::Never}}) {
        const TempPath path(std::string("bench-") + name);
        WriteAheadLog log(WalConfig{.path = path.str(),
                                    .policy = policy,
                                    .batchRecords = 64,
                                    .batchInterval = std::chrono::milliseconds(1000)});

        const auto started = std::chrono::steady_clock::now();
        for (OrderId id = 1; id <= kRecords; ++id) {
            (void)log.append(submitOf(id, Side::Buy, 100, 1, kAlice));
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;

        const double micros =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) /
            kRecords;
        results.push_back(Measured{name, policy, micros, log.syncCount()});
    }

    for (const Measured& measured : results) {
        std::printf("  %-7s %8.2f us/record  %6zu syncs\n", measured.name, measured.microsPerRecord,
                    measured.syncs);
    }

    // The only ordering worth asserting is the one that is true by
    // construction: forcing per record costs strictly more syncs than
    // batching, which costs more than never. Timing itself is reported, not
    // asserted -- on a ramdisk or an NVMe with a write cache the gap can
    // collapse, and that would be a property of the device rather than a bug.
    EXPECT_EQ(results[0].syncs, static_cast<std::size_t>(kRecords));
    EXPECT_LT(results[1].syncs, results[0].syncs);
    EXPECT_EQ(results[2].syncs, 0u);
}

} // namespace
} // namespace exchange::store
