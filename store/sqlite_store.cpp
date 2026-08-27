#include "store/sqlite_store.hpp"

#include "core/fill.hpp"
#include "core/types.hpp"
#include "store/sqlite.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace exchange::store {
namespace {

/// Notional value of a fill, in the integer units prices are quoted in.
[[nodiscard]] std::int64_t notionalOf(const Fill& fill) noexcept {
    return fill.price * static_cast<std::int64_t>(fill.quantity);
}

} // namespace

SqliteStore::SqliteStore(const std::string& path) : database_(path) {
    createSchema();
}

void SqliteStore::createSchema() {
    // STRICT tables, which SQLite has had since 3.37.
    //
    // Without it SQLite applies type *affinity* rather than type checking, so
    // inserting the string 'oops' into an INTEGER column succeeds and stores a
    // string. For a ledger that is intolerable: a balance column that might
    // hold text is not a balance. STRICT makes the declared type a constraint.
    database_.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS accounts (
            account_id INTEGER PRIMARY KEY,
            balance    INTEGER NOT NULL
        ) STRICT;

        CREATE TABLE IF NOT EXISTS fills (
            seq                INTEGER PRIMARY KEY,
            ts                 INTEGER NOT NULL,
            symbol             TEXT    NOT NULL,
            aggressor_order_id INTEGER NOT NULL,
            resting_order_id   INTEGER NOT NULL,
            aggressor_account  INTEGER NOT NULL,
            resting_account    INTEGER NOT NULL,
            aggressor_side     INTEGER NOT NULL,
            price              INTEGER NOT NULL,
            quantity           INTEGER NOT NULL CHECK (quantity > 0)
        ) STRICT;

        CREATE TABLE IF NOT EXISTS ledger (
            entry_id   INTEGER PRIMARY KEY,
            fill_seq   INTEGER NOT NULL REFERENCES fills(seq),
            account_id INTEGER NOT NULL REFERENCES accounts(account_id),
            delta      INTEGER NOT NULL
        ) STRICT;
    )SQL");

    // Indexes are chosen for two specific queries rather than sprinkled on
    // every column, because each one is a second B-tree that every insert has
    // to maintain -- so an index nothing reads is a pure write cost.
    //
    //   fills(aggressor_account, ts) serves the per-account statement:
    //   "my trades, newest first". Composite and in that order, because the
    //   account is an equality filter and ts is the range/ordering -- the
    //   reverse order would make the index useless for the equality.
    //
    //   fills(symbol, ts) serves the tape: "recent trades in this
    //   instrument".
    //
    // seq is already the INTEGER PRIMARY KEY, so it is the rowid and needs no
    // separate index -- SQLite stores the table itself in that order.
    database_.exec(R"SQL(
        CREATE INDEX IF NOT EXISTS idx_fills_account_ts ON fills(aggressor_account, ts);
        CREATE INDEX IF NOT EXISTS idx_fills_symbol_ts  ON fills(symbol, ts);
        CREATE INDEX IF NOT EXISTS idx_ledger_fill      ON ledger(fill_seq);
    )SQL");
}

void SqliteStore::createAccount(AccountId account, std::int64_t openingBalance) {
    Statement insert =
        database_.prepare("INSERT INTO accounts(account_id, balance) VALUES (?1, ?2) "
                          "ON CONFLICT(account_id) DO NOTHING;");
    insert.bind(1, static_cast<std::int64_t>(account));
    insert.bind(2, openingBalance);
    insert.execute();
}

bool SqliteStore::recordFill(Sequence sequence, const Fill& fill, std::int64_t timestamp) {
    const auto seq = static_cast<std::int64_t>(sequence);

    // Idempotence check, inside the same transaction as the write so that two
    // attempts cannot both see "absent" and both insert.
    Transaction transaction(database_);

    {
        Statement existing = database_.prepare("SELECT 1 FROM fills WHERE seq = ?1;");
        existing.bind(1, seq);
        if (existing.step()) {
            transaction.commit();
            return false;
        }
    }

    {
        Statement insert = database_.prepare(
            "INSERT INTO fills(seq, ts, symbol, aggressor_order_id, resting_order_id, "
            "                  aggressor_account, resting_account, aggressor_side, "
            "                  price, quantity) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);");
        insert.bind(1, seq);
        insert.bind(2, timestamp);
        // Single-instrument by design; ARCHITECTURE section 10 names
        // multi-symbol sharding as deliberately out of scope. The column
        // exists so the tape index is meaningful and the schema does not need
        // migrating to add a second instrument.
        insert.bind(3, std::string_view("EXCH"));
        insert.bind(4, static_cast<std::int64_t>(fill.aggressorId));
        insert.bind(5, static_cast<std::int64_t>(fill.restingId));
        insert.bind(6, static_cast<std::int64_t>(fill.aggressorAccount));
        insert.bind(7, static_cast<std::int64_t>(fill.restingAccount));
        insert.bind(8, static_cast<std::int64_t>(fill.aggressorSide));
        insert.bind(9, fill.price);
        insert.bind(10, static_cast<std::int64_t>(fill.quantity));
        insert.execute();
    }

    // Double entry. The buyer pays notional and the seller receives it, so the
    // two postings sum to zero by construction -- which is what makes
    // ledgerImbalance() a real invariant rather than a hopeful one.
    const std::int64_t notional = notionalOf(fill);
    const bool aggressorBuys = fill.aggressorSide == Side::Buy;

    const LedgerEntry aggressorEntry{.account = fill.aggressorAccount,
                                     .delta = aggressorBuys ? -notional : notional};
    const LedgerEntry restingEntry{.account = fill.restingAccount,
                                   .delta = aggressorBuys ? notional : -notional};

    Statement posting =
        database_.prepare("INSERT INTO ledger(fill_seq, account_id, delta) VALUES (?1, ?2, ?3);");
    Statement adjust =
        database_.prepare("UPDATE accounts SET balance = balance + ?2 WHERE account_id = ?1;");

    for (const LedgerEntry& entry : {aggressorEntry, restingEntry}) {
        posting.bind(1, seq);
        posting.bind(2, static_cast<std::int64_t>(entry.account));
        posting.bind(3, entry.delta);
        posting.execute();

        adjust.bind(1, static_cast<std::int64_t>(entry.account));
        adjust.bind(2, entry.delta);
        adjust.execute();
    }

    // Nothing above is visible until this line. A throw anywhere in between
    // takes the Transaction destructor's rollback instead.
    transaction.commit();
    return true;
}

std::optional<std::int64_t> SqliteStore::balanceOf(AccountId account) {
    Statement query = database_.prepare("SELECT balance FROM accounts WHERE account_id = ?1;");
    query.bind(1, static_cast<std::int64_t>(account));
    if (!query.step()) {
        return std::nullopt;
    }
    return query.columnInt(0);
}

std::size_t SqliteStore::fillCount() {
    Statement query = database_.prepare("SELECT COUNT(*) FROM fills;");
    if (!query.step()) {
        return 0;
    }
    return static_cast<std::size_t>(query.columnInt(0));
}

Sequence SqliteStore::lastFillSequence() {
    Statement query = database_.prepare("SELECT COALESCE(MAX(seq), 0) FROM fills;");
    if (!query.step()) {
        return 0;
    }
    return static_cast<Sequence>(query.columnInt(0));
}

std::vector<Fill> SqliteStore::fillsForAccount(AccountId account, std::size_t limit) {
    Statement query =
        database_.prepare("SELECT aggressor_order_id, resting_order_id, price, quantity, "
                          "       aggressor_account, resting_account, aggressor_side "
                          "FROM fills WHERE aggressor_account = ?1 ORDER BY ts DESC LIMIT ?2;");
    query.bind(1, static_cast<std::int64_t>(account));
    query.bind(2, static_cast<std::int64_t>(limit));

    std::vector<Fill> fills;
    while (query.step()) {
        fills.push_back(Fill{
            .aggressorId = static_cast<OrderId>(query.columnInt(0)),
            .restingId = static_cast<OrderId>(query.columnInt(1)),
            .price = query.columnInt(2),
            .quantity = static_cast<Quantity>(query.columnInt(3)),
            .aggressorAccount = static_cast<AccountId>(query.columnInt(4)),
            .restingAccount = static_cast<AccountId>(query.columnInt(5)),
            .aggressorSide = static_cast<Side>(query.columnInt(6)),
        });
    }
    return fills;
}

std::vector<Fill> SqliteStore::recentFills(std::size_t limit) {
    Statement query =
        database_.prepare("SELECT aggressor_order_id, resting_order_id, price, quantity, "
                          "       aggressor_account, resting_account, aggressor_side "
                          "FROM fills WHERE symbol = ?1 ORDER BY ts DESC LIMIT ?2;");
    query.bind(1, std::string_view("EXCH"));
    query.bind(2, static_cast<std::int64_t>(limit));

    std::vector<Fill> fills;
    while (query.step()) {
        fills.push_back(Fill{
            .aggressorId = static_cast<OrderId>(query.columnInt(0)),
            .restingId = static_cast<OrderId>(query.columnInt(1)),
            .price = query.columnInt(2),
            .quantity = static_cast<Quantity>(query.columnInt(3)),
            .aggressorAccount = static_cast<AccountId>(query.columnInt(4)),
            .restingAccount = static_cast<AccountId>(query.columnInt(5)),
            .aggressorSide = static_cast<Side>(query.columnInt(6)),
        });
    }
    return fills;
}

std::int64_t SqliteStore::ledgerImbalance() {
    Statement query = database_.prepare("SELECT COALESCE(SUM(delta), 0) FROM ledger;");
    if (!query.step()) {
        return 0;
    }
    return query.columnInt(0);
}

std::vector<AccountBalance> SqliteStore::allBalances() {
    Statement query =
        database_.prepare("SELECT account_id, balance FROM accounts ORDER BY account_id;");

    std::vector<AccountBalance> balances;
    while (query.step()) {
        balances.push_back(AccountBalance{.account = static_cast<AccountId>(query.columnInt(0)),
                                          .balance = query.columnInt(1)});
    }
    return balances;
}

} // namespace exchange::store
