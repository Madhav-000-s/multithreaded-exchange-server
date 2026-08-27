// The write-ahead log: record framing, torn-tail handling, and the sync
// policies.
//
// The corruption cases are the point. A log that only reads back what it wrote
// cleanly has not been tested for the situation it exists to survive.

#include "core/command.hpp"
#include "core/exceptions.hpp"
#include "core/types.hpp"
#include "store/crc32.hpp"
#include "store/wal_record.hpp"
#include "store/write_ahead_log.hpp"

#include "order_factory.hpp"
#include "temp_path.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace exchange::store {
namespace {

using test::kAlice;
using test::kBob;
using test::TempPath;

[[nodiscard]] Command submitOf(OrderId id, Side side, Price price, Quantity qty,
                               AccountId account) {
    return SubmitCommand{.order = test::limit(id, side, price, qty, account)};
}

[[nodiscard]] std::vector<std::byte> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());

    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

void writeFile(const std::string& path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    for (const std::byte byte : bytes) {
        file.put(static_cast<char>(byte));
    }
}

// ---------------------------------------------------------------------------
// CRC
// ---------------------------------------------------------------------------

TEST(Crc32, MatchesTheKnownIeeeVector) {
    // "123456789" -> 0xCBF43926 is the standard check value for CRC-32/ISO-HDLC.
    // Pinned against the published constant rather than against this
    // implementation's own output, which would only prove it is consistent
    // with itself.
    const std::string input = "123456789";
    std::vector<std::byte> bytes(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        bytes[i] = static_cast<std::byte>(input[i]);
    }

    EXPECT_EQ(crc32(bytes), 0xCBF43926U);
}

TEST(Crc32, DetectsASingleFlippedBit) {
    std::vector<std::byte> bytes(64, std::byte{0xA5});
    const std::uint32_t original = crc32(bytes);

    bytes[31] ^= std::byte{0x01};

    EXPECT_NE(crc32(bytes), original);
}

// ---------------------------------------------------------------------------
// Record codec
// ---------------------------------------------------------------------------

TEST(WalRecord, RoundTripsASubmit) {
    const std::vector<std::byte> encoded =
        encodeRecord(7, submitOf(42, Side::Buy, -250, 900, kAlice));

    const ReadResult read = decodeRecord(encoded);

    ASSERT_EQ(read.status, ReadStatus::Ok);
    ASSERT_TRUE(read.record.has_value());
    EXPECT_EQ(read.record->sequence, 7u);
    EXPECT_EQ(read.consumed, encoded.size());

    ASSERT_TRUE(std::holds_alternative<SubmitCommand>(read.record->command));
    const Order& order = *std::get<SubmitCommand>(read.record->command).order;
    EXPECT_EQ(order.id(), 42u);
    EXPECT_EQ(order.account(), kAlice);
    EXPECT_EQ(order.side(), Side::Buy);
    EXPECT_EQ(order.remaining(), 900u);
    ASSERT_TRUE(order.restingPrice().has_value());
    EXPECT_EQ(*order.restingPrice(), -250) << "negative prices survive the log too";
}

TEST(WalRecord, RoundTripsAnIcebergWithItsDisplaySize) {
    Command command = SubmitCommand{.order = test::iceberg(1, Side::Sell, 100, 500, 25, kBob)};
    const std::vector<std::byte> encoded = encodeRecord(1, command);

    const ReadResult read = decodeRecord(encoded);

    ASSERT_EQ(read.status, ReadStatus::Ok);
    const Order& order = *std::get<SubmitCommand>(read.record->command).order;
    EXPECT_EQ(order.remaining(), 500u);
    EXPECT_EQ(order.visibleQty(), 25u) << "an iceberg recovered as a plain limit would leak size";
}

TEST(WalRecord, RoundTripsCancelAndModify) {
    const std::vector<std::byte> cancel = encodeRecord(1, CancelCommand{.id = 5});
    const std::vector<std::byte> modify =
        encodeRecord(2, ModifyCommand{.id = 5, .quantity = 10, .price = 99});
    const std::vector<std::byte> modifyNoPrice =
        encodeRecord(3, ModifyCommand{.id = 5, .quantity = 10, .price = std::nullopt});

    EXPECT_EQ(std::get<CancelCommand>(decodeRecord(cancel).record->command).id, 5u);
    EXPECT_EQ(*std::get<ModifyCommand>(decodeRecord(modify).record->command).price, 99);
    EXPECT_FALSE(
        std::get<ModifyCommand>(decodeRecord(modifyNoPrice).record->command).price.has_value());
}

TEST(WalRecord, ReportsAnIncompleteTailRatherThanFailing) {
    // The normal state of a log whose writer was killed. Not an error: the
    // caller stops here and keeps everything before it.
    std::vector<std::byte> encoded = encodeRecord(1, CancelCommand{.id = 5});
    encoded.pop_back();

    EXPECT_EQ(decodeRecord(encoded).status, ReadStatus::Incomplete);
}

TEST(WalRecord, ReportsIncompleteForALoneHeader) {
    std::vector<std::byte> encoded = encodeRecord(1, CancelCommand{.id = 5});
    encoded.resize(kRecordHeaderSize);

    EXPECT_EQ(decodeRecord(encoded).status, ReadStatus::Incomplete);
}

TEST(WalRecord, DetectsAPlausibleButCorruptPayload) {
    // The case a length check alone cannot catch: the record is structurally
    // fine and semantically garbage. Without a checksum this would replay as
    // a fabricated order.
    std::vector<std::byte> encoded = encodeRecord(1, submitOf(1, Side::Buy, 100, 10, kAlice));
    encoded[kRecordHeaderSize + 4] ^= std::byte{0xFF};

    EXPECT_EQ(decodeRecord(encoded).status, ReadStatus::Corrupt);
}

TEST(WalRecord, RejectsAnImplausibleLength) {
    std::vector<std::byte> encoded = encodeRecord(1, CancelCommand{.id = 5});
    // A length beyond the cap means damage, not a very large order.
    encoded[0] = std::byte{0xFF};
    encoded[1] = std::byte{0xFF};
    encoded[2] = std::byte{0xFF};
    encoded[3] = std::byte{0xFF};

    EXPECT_EQ(decodeRecord(encoded).status, ReadStatus::Corrupt);
}

// ---------------------------------------------------------------------------
// The log itself
// ---------------------------------------------------------------------------

TEST(WriteAheadLog, AppendsAndReplaysInOrder) {
    const TempPath path("wal-order");

    {
        WriteAheadLog log(WalConfig{.path = path.str(), .policy = SyncPolicy::Always});
        for (OrderId id = 1; id <= 5; ++id) {
            EXPECT_EQ(log.append(submitOf(id, Side::Buy, 100, 1, kAlice)), id);
        }
    }

    const WriteAheadLog::ReplayResult replayed = WriteAheadLog::replay(path.str());

    ASSERT_EQ(replayed.records.size(), 5u);
    EXPECT_FALSE(replayed.truncated);
    for (std::size_t i = 0; i < replayed.records.size(); ++i) {
        EXPECT_EQ(replayed.records[i].sequence, i + 1);
    }
}

TEST(WriteAheadLog, ReplayOfAMissingFileIsEmptyRatherThanAnError) {
    const TempPath path("wal-absent");

    const WriteAheadLog::ReplayResult replayed = WriteAheadLog::replay(path.str() + ".nope");

    EXPECT_TRUE(replayed.records.empty());
    EXPECT_FALSE(replayed.truncated);
}

TEST(WriteAheadLog, ContinuesTheSequenceAcrossAReopen) {
    // Sequence numbers must stay unique across a restart, or a recovered log
    // reads as two overlapping histories.
    const TempPath path("wal-reopen");

    {
        WriteAheadLog log(WalConfig{.path = path.str(), .policy = SyncPolicy::Always});
        (void)log.append(CancelCommand{.id = 1});
        (void)log.append(CancelCommand{.id = 2});
    }
    {
        WriteAheadLog log(WalConfig{.path = path.str(), .policy = SyncPolicy::Always});
        EXPECT_EQ(log.append(CancelCommand{.id = 3}), 3u);
    }

    EXPECT_EQ(WriteAheadLog::replay(path.str()).records.size(), 3u);
}

TEST(WriteAheadLog, StopsAtATornTailAndKeepsWhatCameBefore) {
    // Simulates a kill mid-write: everything up to the damage is sound and
    // must be kept; everything after is discarded.
    const TempPath path("wal-torn");

    {
        WriteAheadLog log(WalConfig{.path = path.str(), .policy = SyncPolicy::Always});
        for (OrderId id = 1; id <= 4; ++id) {
            (void)log.append(submitOf(id, Side::Buy, 100, 1, kAlice));
        }
    }

    std::vector<std::byte> contents = readFile(path.str());
    const std::size_t intact = contents.size();
    // Append half a record.
    const std::vector<std::byte> partial = encodeRecord(99, CancelCommand{.id = 7});
    contents.insert(contents.end(), partial.begin(),
                    partial.begin() + static_cast<std::ptrdiff_t>(partial.size() / 2));
    writeFile(path.str(), contents);

    const WriteAheadLog::ReplayResult replayed = WriteAheadLog::replay(path.str());

    EXPECT_EQ(replayed.records.size(), 4u) << "the four complete records survive";
    EXPECT_TRUE(replayed.truncated);
    EXPECT_EQ(replayed.validBytes, intact);
}

TEST(WriteAheadLog, TruncationRemovesTheDamagedTail) {
    const TempPath path("wal-truncate");

    {
        WriteAheadLog log(WalConfig{.path = path.str(), .policy = SyncPolicy::Always});
        (void)log.append(CancelCommand{.id = 1});
    }

    std::vector<std::byte> contents = readFile(path.str());
    const std::size_t intact = contents.size();
    contents.push_back(std::byte{0xDE});
    contents.push_back(std::byte{0xAD});
    writeFile(path.str(), contents);

    WriteAheadLog::truncateTo(path.str(), intact);

    const WriteAheadLog::ReplayResult replayed = WriteAheadLog::replay(path.str());
    EXPECT_EQ(replayed.records.size(), 1u);
    EXPECT_FALSE(replayed.truncated) << "and the log is clean afterwards";
}

// ---------------------------------------------------------------------------
// Sync policy -- the durability/throughput trade
// ---------------------------------------------------------------------------

TEST(WalSyncPolicy, AlwaysForcesOncePerRecord) {
    const TempPath path("wal-always");
    WriteAheadLog log(WalConfig{.path = path.str(), .policy = SyncPolicy::Always});

    for (int i = 0; i < 20; ++i) {
        (void)log.append(CancelCommand{.id = static_cast<OrderId>(i)});
    }

    EXPECT_EQ(log.syncCount(), 20u) << "nothing is acknowledged that is not on the platter";
}

TEST(WalSyncPolicy, NeverForcesAtAllUntilFlush) {
    const TempPath path("wal-never");
    WriteAheadLog log(WalConfig{.path = path.str(), .policy = SyncPolicy::Never});

    for (int i = 0; i < 20; ++i) {
        (void)log.append(CancelCommand{.id = static_cast<OrderId>(i)});
    }
    EXPECT_EQ(log.syncCount(), 0u);

    // The data still survives process death -- it is in the kernel's page
    // cache, not the process's memory. It is power loss this does not survive.
    log.flush();
    EXPECT_EQ(log.syncCount(), 1u);
}

TEST(WalSyncPolicy, BatchForcesOnceEveryNRecords) {
    const TempPath path("wal-batch");
    WriteAheadLog log(WalConfig{.path = path.str(),
                                .policy = SyncPolicy::Batch,
                                .batchRecords = 10,
                                // Long enough that the count, not the clock,
                                // is what triggers here.
                                .batchInterval = std::chrono::milliseconds(60'000)});

    for (int i = 0; i < 30; ++i) {
        (void)log.append(CancelCommand{.id = static_cast<OrderId>(i)});
    }

    EXPECT_EQ(log.syncCount(), 3u) << "a bounded, stated loss window rather than an unknown one";
    EXPECT_EQ(log.recordsWritten(), 30u);
}

TEST(WalSyncPolicy, EveryPolicyWritesTheSameRecords) {
    // The policies differ in when data reaches the platter, never in what is
    // written. A recovered log must not depend on how it was flushed.
    const TempPath always("wal-cmp-always");
    const TempPath batch("wal-cmp-batch");
    const TempPath never("wal-cmp-never");

    for (const auto& [path, policy] :
         {std::pair{always.str(), SyncPolicy::Always}, std::pair{batch.str(), SyncPolicy::Batch},
          std::pair{never.str(), SyncPolicy::Never}}) {
        WriteAheadLog log(WalConfig{.path = path, .policy = policy});
        for (OrderId id = 1; id <= 8; ++id) {
            (void)log.append(submitOf(id, Side::Buy, 100, 1, kAlice));
        }
        log.flush();
    }

    EXPECT_EQ(readFile(always.str()), readFile(batch.str()));
    EXPECT_EQ(readFile(always.str()), readFile(never.str()));
}

} // namespace
} // namespace exchange::store
