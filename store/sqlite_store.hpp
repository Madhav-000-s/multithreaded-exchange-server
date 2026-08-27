#pragma once

#include "core/fill.hpp"
#include "core/types.hpp"
#include "store/sqlite.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace exchange::store {

/// One side of a double-entry posting.
struct LedgerEntry {
    AccountId account{};
    /// Signed, in the same integer units as Price x Quantity. Positive is a
    /// credit to the account, negative a debit.
    std::int64_t delta{};
};

struct AccountBalance {
    AccountId account{};
    std::int64_t balance{};
};

/// Accounts, executed fills, and the ledger that ties them together.
///
/// **Owned outright by one thread and never shared**, which is what licenses
/// the SQLITE_OPEN_NOMUTEX in Database. The engine hands fills over; nothing
/// else touches this.
///
/// SQLite is a *derived* view here, not the authority. The write-ahead log is
/// the record of what happened; this is the queryable projection of it, and
/// recovery rebuilds it from the log rather than trusting it. That is why its
/// `synchronous` pragma can be NORMAL rather than FULL -- losing the tail of
/// this database costs a replay, not the data.
class SqliteStore {
public:
    /// @param path filesystem path, or ":memory:" for tests.
    explicit SqliteStore(const std::string& path);

    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;
    SqliteStore(SqliteStore&&) = delete;
    SqliteStore& operator=(SqliteStore&&) = delete;
    ~SqliteStore() = default;

    /// Creates an account, or does nothing if it exists.
    void createAccount(AccountId account, std::int64_t openingBalance);

    /// Records a fill and its two ledger postings.
    ///
    /// **Atomic by construction.** The fill row, the buyer's debit and the
    /// seller's credit are one transaction: all three land or none do. A
    /// partially applied trade -- cash leaving one account without arriving
    /// at the other -- is the failure this exists to make impossible, and it
    /// is not detectable after the fact without a full reconciliation.
    ///
    /// Idempotent on `sequence`: replaying a log that was already partly
    /// persisted must not double-count, which is exactly what recovery does.
    ///
    /// @return false if this sequence was already recorded.
    bool recordFill(Sequence sequence, const Fill& fill, std::int64_t timestamp);

    [[nodiscard]] std::optional<std::int64_t> balanceOf(AccountId account);

    [[nodiscard]] std::size_t fillCount();

    /// Highest fill sequence persisted, for reconciling against the WAL.
    [[nodiscard]] Sequence lastFillSequence();

    /// Per-account statement, newest first. Served by the
    /// `fills(account, ts)` index.
    [[nodiscard]] std::vector<Fill> fillsForAccount(AccountId account, std::size_t limit);

    /// The tape. Served by the `fills(symbol, ts)` index.
    [[nodiscard]] std::vector<Fill> recentFills(std::size_t limit);

    /// Sum of every ledger posting.
    ///
    /// **Must always be zero.** Double-entry's whole purpose is that this is
    /// checkable: money moves between accounts and is never created, so any
    /// non-zero total is a bug that a per-account balance check would miss.
    [[nodiscard]] std::int64_t ledgerImbalance();

    [[nodiscard]] std::vector<AccountBalance> allBalances();

    [[nodiscard]] Database& database() noexcept { return database_; }

private:
    void createSchema();

    Database database_;
};

} // namespace exchange::store
