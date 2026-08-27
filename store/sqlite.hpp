#pragma once

#include "core/exceptions.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace exchange::store {

/// RAII around the two SQLite handle types.
///
/// Both are C handles with a dedicated close function, which is exactly what
/// `unique_ptr` with a custom deleter is for. Nothing here is written by hand
/// beyond the deleter -- no destructor, no rule of five, no `finalize` that a
/// later `return` can skip past.
///
/// The `sqlite3_stmt` case matters more than it looks. A statement that is not
/// finalised holds a read transaction open, so one leaked statement can block
/// every subsequent write with SQLITE_BUSY -- a failure that appears far from
/// its cause and looks like a locking problem rather than a lifetime one.
namespace detail {

struct DatabaseDeleter {
    void operator()(sqlite3* handle) const noexcept {
        // close_v2, not close: close() refuses while statements are still
        // alive, which in a destructor would mean silently leaking the whole
        // connection. close_v2 marks it zombie and reclaims it once the last
        // statement goes.
        sqlite3_close_v2(handle);
    }
};

struct StatementDeleter {
    void operator()(sqlite3_stmt* handle) const noexcept { sqlite3_finalize(handle); }
};

} // namespace detail

using DatabaseHandle = std::unique_ptr<sqlite3, detail::DatabaseDeleter>;
using StatementHandle = std::unique_ptr<sqlite3_stmt, detail::StatementDeleter>;

/// A prepared statement, reusable across executions.
///
/// Prepared and bound rather than composed by string concatenation. That is
/// the SQL-injection answer, but it is also the performance one: preparing
/// parses and plans the statement once, and a fill insert on the hot path
/// should not re-plan itself several thousand times a second.
class Statement {
public:
    Statement(sqlite3& database, std::string_view sql);

    void bind(int index, std::int64_t value);
    void bind(int index, std::string_view value);

    /// Runs a statement that returns no rows.
    void execute();

    /// Advances to the next row. @return false when the result set ends.
    [[nodiscard]] bool step();

    [[nodiscard]] std::int64_t columnInt(int index) const noexcept;
    [[nodiscard]] std::string columnText(int index) const;

    /// Clears bindings and rewinds, so the statement can be run again.
    void reset() noexcept;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return handle_.get(); }

private:
    StatementHandle handle_;
    sqlite3* database_;
};

/// A connection, plus the pragmas that make it behave the way this system
/// needs.
class Database {
public:
    /// @param path a filesystem path, or ":memory:" for a private database.
    explicit Database(const std::string& path);

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept = default;
    Database& operator=(Database&&) noexcept = default;
    ~Database() = default;

    /// Runs one or more statements with no parameters and no results.
    void exec(std::string_view sql);

    [[nodiscard]] Statement prepare(std::string_view sql) { return Statement(*handle_, sql); }

    [[nodiscard]] std::int64_t lastInsertRowId() const noexcept {
        return sqlite3_last_insert_rowid(handle_.get());
    }

    [[nodiscard]] sqlite3& handle() const noexcept { return *handle_; }

private:
    DatabaseHandle handle_;
};

/// Scoped transaction with rollback on destruction.
///
/// The double-entry requirement is that a debit and its matching credit either
/// both land or neither does. Left to hand-written begin/commit, an early
/// return or a throw between them leaves the transaction open and the ledger
/// unbalanced -- and the balance error would surface much later, in a
/// reconciliation, with nothing left to point at the cause.
///
/// `BEGIN IMMEDIATE` rather than plain `BEGIN`: a deferred transaction takes
/// its write lock lazily, at the first write, so two writers can each start
/// happily and then have one fail with SQLITE_BUSY part way through. Taking
/// the lock up front converts that into a clean wait at the beginning.
class Transaction {
public:
    explicit Transaction(Database& database);

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    /// Rolls back unless commit() was called.
    ~Transaction();

    void commit();

private:
    Database* database_;
    bool committed_{false};
};

/// Turns a SQLite result code into a StorageError carrying the message.
void checkResult(sqlite3& database, int code, const char* what);

} // namespace exchange::store
