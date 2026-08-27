// SQLite: schema, double-entry postings, transaction atomicity and the index
// choices.

#include "core/fill.hpp"
#include "core/types.hpp"
#include "store/sqlite.hpp"
#include "store/sqlite_store.hpp"

#include "temp_path.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace exchange::store {
namespace {

using test::TempPath;

constexpr AccountId kBuyer = 100;
constexpr AccountId kSeller = 200;

[[nodiscard]] Fill fillOf(OrderId aggressor, OrderId resting, Price price, Quantity qty,
                          Side side) {
    return Fill{.aggressorId = aggressor,
                .restingId = resting,
                .price = price,
                .quantity = qty,
                .aggressorAccount = side == Side::Buy ? kBuyer : kSeller,
                .restingAccount = side == Side::Buy ? kSeller : kBuyer,
                .aggressorSide = side};
}

[[nodiscard]] SqliteStore makeStore() {
    // In-memory: the schema and the logic are what is under test, and a
    // private database per test makes them independent.
    return SqliteStore(":memory:");
}

// ---------------------------------------------------------------------------

TEST(SqliteStore, CreatesAnAccountIdempotently) {
    SqliteStore store = makeStore();

    store.createAccount(kBuyer, 1000);
    store.createAccount(kBuyer, 9999); // second call must not overwrite

    ASSERT_TRUE(store.balanceOf(kBuyer).has_value());
    EXPECT_EQ(*store.balanceOf(kBuyer), 1000);
    EXPECT_FALSE(store.balanceOf(kSeller).has_value());
}

TEST(SqliteStore, RecordsAFillAndItsTwoPostings) {
    SqliteStore store = makeStore();
    store.createAccount(kBuyer, 10'000);
    store.createAccount(kSeller, 10'000);

    // Buyer aggresses: pays 250 x 4 = 1000.
    EXPECT_TRUE(store.recordFill(1, fillOf(1, 2, 250, 4, Side::Buy), 1'000));

    EXPECT_EQ(store.fillCount(), 1u);
    EXPECT_EQ(*store.balanceOf(kBuyer), 9'000);
    EXPECT_EQ(*store.balanceOf(kSeller), 11'000);
}

TEST(SqliteStore, TheLedgerAlwaysBalances) {
    // Double entry's whole purpose: money moves between accounts and is never
    // created, so the sum of every posting is checkable. A per-account balance
    // check would not catch a posting that went missing.
    SqliteStore store = makeStore();
    store.createAccount(kBuyer, 100'000);
    store.createAccount(kSeller, 100'000);

    for (Sequence seq = 1; seq <= 50; ++seq) {
        const Side side = (seq % 2 == 0) ? Side::Buy : Side::Sell;
        EXPECT_TRUE(
            store.recordFill(seq, fillOf(seq, seq + 1000, 100 + static_cast<Price>(seq), seq, side),
                             static_cast<std::int64_t>(seq)));
    }

    EXPECT_EQ(store.ledgerImbalance(), 0) << "a non-zero total is money invented or destroyed";
    EXPECT_EQ(store.fillCount(), 50u);
}

TEST(SqliteStore, SellerAggressingMovesCashTheOtherWay) {
    SqliteStore store = makeStore();
    store.createAccount(kBuyer, 10'000);
    store.createAccount(kSeller, 10'000);

    // Seller aggresses: receives 250 x 4.
    EXPECT_TRUE(store.recordFill(1, fillOf(1, 2, 250, 4, Side::Sell), 1'000));

    EXPECT_EQ(*store.balanceOf(kSeller), 11'000);
    EXPECT_EQ(*store.balanceOf(kBuyer), 9'000);
    EXPECT_EQ(store.ledgerImbalance(), 0);
}

TEST(SqliteStore, RecordingTheSameSequenceTwiceIsIgnored) {
    // Idempotence is not a nicety here: recovery replays a log that may have
    // been partly persisted before the crash, so double-counting the ledger is
    // the default failure without it.
    SqliteStore store = makeStore();
    store.createAccount(kBuyer, 10'000);
    store.createAccount(kSeller, 10'000);

    EXPECT_TRUE(store.recordFill(1, fillOf(1, 2, 250, 4, Side::Buy), 1'000));
    EXPECT_FALSE(store.recordFill(1, fillOf(1, 2, 250, 4, Side::Buy), 1'000));

    EXPECT_EQ(store.fillCount(), 1u);
    EXPECT_EQ(*store.balanceOf(kBuyer), 9'000) << "the balance moved once, not twice";
}

TEST(SqliteStore, AFailedFillLeavesNothingBehind) {
    // The atomicity requirement made concrete: the fill row references an
    // account that does not exist, so the foreign key rejects the ledger
    // posting -- and the fill row must not survive on its own.
    SqliteStore store = makeStore();
    store.createAccount(kBuyer, 10'000);
    // kSeller deliberately absent.

    EXPECT_THROW((void)store.recordFill(1, fillOf(1, 2, 250, 4, Side::Buy), 1'000), StorageError);

    EXPECT_EQ(store.fillCount(), 0u) << "the transaction rolled back rather than half-applying";
    EXPECT_EQ(*store.balanceOf(kBuyer), 10'000);
    EXPECT_EQ(store.ledgerImbalance(), 0);
}

TEST(SqliteStore, ReportsTheHighestPersistedSequence) {
    SqliteStore store = makeStore();
    store.createAccount(kBuyer, 10'000);
    store.createAccount(kSeller, 10'000);

    EXPECT_EQ(store.lastFillSequence(), 0u) << "an empty database has recovered nothing";

    (void)store.recordFill(5, fillOf(1, 2, 100, 1, Side::Buy), 1);
    (void)store.recordFill(9, fillOf(3, 4, 100, 1, Side::Buy), 2);

    EXPECT_EQ(store.lastFillSequence(), 9u);
}

// ---------------------------------------------------------------------------
// Queries and the indexes that serve them
// ---------------------------------------------------------------------------

TEST(SqliteStore, ServesThePerAccountStatement) {
    SqliteStore store = makeStore();
    store.createAccount(kBuyer, 100'000);
    store.createAccount(kSeller, 100'000);

    for (Sequence seq = 1; seq <= 10; ++seq) {
        const Side side = seq <= 6 ? Side::Buy : Side::Sell;
        (void)store.recordFill(seq, fillOf(seq, seq + 100, 100, 1, side),
                               static_cast<std::int64_t>(seq));
    }

    const std::vector<Fill> statement = store.fillsForAccount(kBuyer, 100);

    EXPECT_EQ(statement.size(), 6u) << "only the trades this account aggressed";
    EXPECT_EQ(statement.front().aggressorId, 6u) << "newest first";
}

TEST(SqliteStore, ServesTheTape) {
    SqliteStore store = makeStore();
    store.createAccount(kBuyer, 100'000);
    store.createAccount(kSeller, 100'000);

    for (Sequence seq = 1; seq <= 20; ++seq) {
        (void)store.recordFill(seq, fillOf(seq, seq + 100, 100, 1, Side::Buy),
                               static_cast<std::int64_t>(seq));
    }

    const std::vector<Fill> tape = store.recentFills(5);

    EXPECT_EQ(tape.size(), 5u);
    EXPECT_EQ(tape.front().aggressorId, 20u);
}

TEST(SqliteStore, TheStatementQueryActuallyUsesItsIndex) {
    // An index nothing reaches is a pure write cost -- a second B-tree
    // maintained on every insert for nothing. EXPLAIN QUERY PLAN is how that
    // is checked rather than assumed.
    SqliteStore store = makeStore();
    store.createAccount(kBuyer, 1000);

    Statement plan = store.database().prepare(
        "EXPLAIN QUERY PLAN "
        "SELECT seq FROM fills WHERE aggressor_account = 1 ORDER BY ts DESC LIMIT 10;");

    std::string detail;
    while (plan.step()) {
        detail += plan.columnText(3);
    }

    EXPECT_NE(detail.find("idx_fills_account_ts"), std::string::npos) << "plan was: " << detail;
    EXPECT_EQ(detail.find("SCAN"), std::string::npos)
        << "a full scan means the index is not being used: " << detail;
}

TEST(SqliteStore, TheTapeQueryActuallyUsesItsIndex) {
    SqliteStore store = makeStore();

    Statement plan = store.database().prepare(
        "EXPLAIN QUERY PLAN "
        "SELECT seq FROM fills WHERE symbol = 'EXCH' ORDER BY ts DESC LIMIT 10;");

    std::string detail;
    while (plan.step()) {
        detail += plan.columnText(3);
    }

    EXPECT_NE(detail.find("idx_fills_symbol_ts"), std::string::npos) << "plan was: " << detail;
}

// ---------------------------------------------------------------------------
// Schema strictness
// ---------------------------------------------------------------------------

TEST(SqliteStore, StrictTablesRejectTheWrongType) {
    // Without STRICT, SQLite applies type affinity rather than type checking
    // and would store the string. A balance column that might hold text is not
    // a balance.
    SqliteStore store = makeStore();

    EXPECT_THROW(store.database().exec("INSERT INTO accounts VALUES (1, 'not a number');"),
                 StorageError);
}

TEST(SqliteStore, TheQuantityCheckConstraintHolds) {
    SqliteStore store = makeStore();

    EXPECT_THROW(store.database().exec(
                     "INSERT INTO fills VALUES (1, 1, 'EXCH', 1, 2, 100, 200, 0, 250, 0);"),
                 StorageError)
        << "a zero-quantity fill is not a trade";
}

TEST(SqliteStore, ForeignKeysAreEnforced) {
    // PRAGMA foreign_keys is off by default in SQLite, so this checks the
    // pragma was actually applied rather than that SQLite supports the feature.
    SqliteStore store = makeStore();

    EXPECT_THROW(store.database().exec("INSERT INTO ledger(fill_seq, account_id, delta) "
                                       "VALUES (999, 999, 100);"),
                 StorageError);
}

TEST(SqliteStore, SurvivesReopeningAnExistingDatabase) {
    const TempPath path("sqlite-reopen");

    {
        SqliteStore store(path.str());
        store.createAccount(kBuyer, 500);
        store.createAccount(kSeller, 500);
        (void)store.recordFill(1, fillOf(1, 2, 100, 2, Side::Buy), 1);
    }
    {
        SqliteStore store(path.str());
        EXPECT_EQ(store.fillCount(), 1u);
        EXPECT_EQ(*store.balanceOf(kBuyer), 300);
        EXPECT_EQ(store.ledgerImbalance(), 0);
    }
}

} // namespace
} // namespace exchange::store
