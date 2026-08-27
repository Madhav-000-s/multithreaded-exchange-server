#pragma once

#include "core/book.hpp"
#include "core/types.hpp"
#include "store/sqlite_store.hpp"
#include "store/write_ahead_log.hpp"

#include <cstddef>
#include <string>

namespace exchange::store {

struct RecoveryReport {
    /// Records read from the log and applied.
    std::size_t recordsReplayed{0};

    /// Records the book declined -- a cancel for an order that had already
    /// filled, say. Expected in small numbers: the log records what was
    /// *asked*, and a command can be legitimately refused on replay exactly
    /// as it was at run time.
    std::size_t recordsRejected{0};

    /// Fills reproduced by the replay.
    std::size_t fillsReplayed{0};

    /// Fills written to SQLite during recovery, i.e. those the crash lost.
    std::size_t fillsPersisted{0};

    /// The log's tail was damaged and has been trimmed.
    bool logTruncated{false};

    /// Bytes retained after trimming.
    std::size_t validBytes{0};

    /// Highest sequence number recovered.
    Sequence lastSequence{0};
};

/// Rebuilds the book from the write-ahead log.
///
/// **Replays through `applyCommand`, the same function the engine thread calls
/// at run time.** That is the property that makes recovery trustworthy rather
/// than merely present. A separate replay implementation would eventually
/// disagree with the live one -- a subtly different self-match rule, a
/// different iceberg refresh -- and the disagreement would produce a recovered
/// book that never existed, discovered long after the crash that caused it.
///
/// Ordering: the log is the authority, SQLite is a derived view. So the book
/// is rebuilt first, then SQLite is brought forward to match. Fills already
/// persisted are skipped by sequence number, which is why `recordFill` is
/// idempotent -- a crash between writing the log record and writing the
/// database row is the normal case, not an exotic one.
///
/// A damaged tail is trimmed rather than skipped over. A gap in a command log
/// is not a missing row: replaying what follows a hole produces a different
/// history. Everything up to the damage is sound and is kept; everything after
/// is discarded, and the client was never acknowledged for it under any sync
/// policy that promised durability.
[[nodiscard]] RecoveryReport recover(const std::string& walPath, Book& book, SqliteStore& store);

/// Replays into a book without touching any database. Used by tests and by the
/// crash harness, which cares only about the reconstructed book.
[[nodiscard]] RecoveryReport recoverBookOnly(const std::string& walPath, Book& book);

} // namespace exchange::store
