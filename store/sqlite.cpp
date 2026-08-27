#include "store/sqlite.hpp"

#include "core/exceptions.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace exchange::store {

void checkResult(sqlite3& database, int code, const char* what) {
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) {
        return;
    }
    throw StorageError(std::string(what) + ": " + sqlite3_errmsg(&database));
}

// ---------------------------------------------------------------------------

Database::Database(const std::string& path) {
    sqlite3* raw = nullptr;

    // SQLITE_OPEN_NOMUTEX opts out of SQLite's own serialisation.
    //
    // The library is compiled threadsafe, so by default every API call takes
    // an internal mutex. This connection is owned outright by one thread and
    // never shared -- ARCHITECTURE section 5 -- so that lock protects against
    // a scenario the ownership model already makes impossible, and costs a
    // lock acquisition on every call to buy it.
    //
    // Disabling a safety feature is worth being deliberate about: it is only
    // sound *because* of the ownership rule, and it would be a data race the
    // moment a second thread touched this handle.
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;

    const int opened = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    handle_.reset(raw);
    if (opened != SQLITE_OK) {
        // The handle is returned even on failure so the message can be read,
        // and unique_ptr already owns it by this point.
        throw StorageError("open database '" + path +
                           "': " + (raw != nullptr ? sqlite3_errmsg(raw) : "out of memory"));
    }

    // Wait rather than fail immediately when another connection holds the
    // write lock. Without this, contention surfaces as SQLITE_BUSY errors the
    // caller has to retry by hand.
    sqlite3_busy_timeout(handle_.get(), 5000);

    exec("PRAGMA journal_mode = WAL;"
         // SQLite's own write-ahead log, unrelated to this project's. It lets
         // readers proceed during a write instead of blocking, which is what
         // makes the per-account statement query usable while fills are
         // landing.
         "PRAGMA synchronous = NORMAL;"
         // Under WAL, NORMAL forces at checkpoints rather than at every
         // commit. Durability against power loss for the *database* comes
         // from this project's own WAL, which is the authority on what
         // happened; SQLite here is a derived view that recovery rebuilds.
         "PRAGMA foreign_keys = ON;");
}

void Database::exec(std::string_view sql) {
    char* message = nullptr;
    const std::string statement(sql);
    const int code = sqlite3_exec(handle_.get(), statement.c_str(), nullptr, nullptr, &message);

    if (code != SQLITE_OK) {
        const std::string detail = message != nullptr ? message : "unknown error";
        sqlite3_free(message);
        throw StorageError("exec failed: " + detail);
    }
    sqlite3_free(message);
}

// ---------------------------------------------------------------------------

Statement::Statement(sqlite3& database, std::string_view sql) : database_(&database) {
    sqlite3_stmt* raw = nullptr;
    const int code =
        sqlite3_prepare_v2(&database, sql.data(), static_cast<int>(sql.size()), &raw, nullptr);
    handle_.reset(raw);
    checkResult(database, code, "prepare");
}

void Statement::bind(int index, std::int64_t value) {
    checkResult(*database_, sqlite3_bind_int64(handle_.get(), index, value), "bind int");
}

void Statement::bind(int index, std::string_view value) {
    // SQLITE_TRANSIENT tells SQLite to copy the text immediately. The
    // alternative, SQLITE_STATIC, promises the buffer outlives the statement
    // -- a promise a string_view argument cannot make.
    checkResult(*database_,
                sqlite3_bind_text(handle_.get(), index, value.data(),
                                  static_cast<int>(value.size()), SQLITE_TRANSIENT),
                "bind text");
}

void Statement::execute() {
    const int code = sqlite3_step(handle_.get());
    if (code != SQLITE_DONE) {
        checkResult(*database_, code, "execute");
    }
    reset();
}

bool Statement::step() {
    const int code = sqlite3_step(handle_.get());
    if (code == SQLITE_ROW) {
        return true;
    }
    if (code == SQLITE_DONE) {
        return false;
    }
    checkResult(*database_, code, "step");
    return false;
}

std::int64_t Statement::columnInt(int index) const noexcept {
    return sqlite3_column_int64(handle_.get(), index);
}

std::string Statement::columnText(int index) const {
    const unsigned char* text = sqlite3_column_text(handle_.get(), index);
    if (text == nullptr) {
        return {};
    }
    const int bytes = sqlite3_column_bytes(handle_.get(), index);
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

void Statement::reset() noexcept {
    // Both are needed and they do different things: reset rewinds the program
    // counter, clear_bindings drops the parameter values. Reusing a statement
    // without clearing leaves stale bindings for any parameter the next call
    // happens not to set.
    sqlite3_reset(handle_.get());
    sqlite3_clear_bindings(handle_.get());
}

// ---------------------------------------------------------------------------

Transaction::Transaction(Database& database) : database_(&database) {
    database_->exec("BEGIN IMMEDIATE;");
}

Transaction::~Transaction() {
    if (committed_) {
        return;
    }
    try {
        database_->exec("ROLLBACK;");
    } catch (const StorageError&) {
        // A destructor must not throw, and there is no caller left to inform.
        // The transaction is abandoned either way: SQLite rolls back an open
        // transaction when the connection closes.
    }
}

void Transaction::commit() {
    database_->exec("COMMIT;");
    committed_ = true;
}

} // namespace exchange::store
