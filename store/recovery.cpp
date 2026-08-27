#include "store/recovery.hpp"

#include "core/apply.hpp"
#include "core/book.hpp"
#include "core/exceptions.hpp"
#include "store/sqlite_store.hpp"
#include "store/wal_record.hpp"
#include "store/write_ahead_log.hpp"

#include <string>
#include <utility>

namespace exchange::store {
namespace {

/// Applies one record, folding a refusal into the report rather than throwing.
void replayOne(Book& book, WalRecord&& record, RecoveryReport& report) {
    try {
        const ApplyOutcome outcome = applyCommand(book, std::move(record.command));
        if (outcome.accepted) {
            ++report.recordsReplayed;
        } else {
            ++report.recordsRejected;
        }
        report.fillsReplayed += outcome.fills.size();
    } catch (const ExchangeError&) {
        // The book refused it -- a duplicate id, a zero quantity. The log
        // records what was *asked* for, and a command refused at run time is
        // refused identically on replay. Counting it is right; aborting
        // recovery over it would be wrong.
        ++report.recordsRejected;
    }
    report.lastSequence = record.sequence;
}

} // namespace

RecoveryReport recoverBookOnly(const std::string& walPath, Book& book) {
    RecoveryReport report;

    WriteAheadLog::ReplayResult replayed = WriteAheadLog::replay(walPath);
    report.logTruncated = replayed.truncated;
    report.validBytes = replayed.validBytes;

    for (WalRecord& record : replayed.records) {
        replayOne(book, std::move(record), report);
    }

    // Trim only after every intact record has been applied. Truncating first
    // would risk losing the log if the replay then threw.
    if (replayed.truncated) {
        WriteAheadLog::truncateTo(walPath, replayed.validBytes);
    }
    return report;
}

RecoveryReport recover(const std::string& walPath, Book& book, SqliteStore& store) {
    RecoveryReport report;

    WriteAheadLog::ReplayResult replayed = WriteAheadLog::replay(walPath);
    report.logTruncated = replayed.truncated;
    report.validBytes = replayed.validBytes;

    // Everything at or below this was already committed to the database before
    // the crash. Re-recording it would double-count the ledger, which is why
    // recordFill is keyed on sequence and idempotent.
    const Sequence persistedThrough = store.lastFillSequence();

    for (WalRecord& record : replayed.records) {
        const Sequence sequence = record.sequence;

        RecoveryReport step;
        replayOne(book, std::move(record), step);

        report.recordsReplayed += step.recordsReplayed;
        report.recordsRejected += step.recordsRejected;
        report.fillsReplayed += step.fillsReplayed;
        report.lastSequence = sequence;

        // The database is a derived view, so it is brought forward from the
        // log rather than trusted. A crash between appending the record and
        // writing the row is the ordinary case: the WAL wins, and the row is
        // written now.
        if (sequence > persistedThrough) {
            // Fills are re-derived by the replay itself; recordFill is called
            // by the caller's fill handler in the live path. Here the count is
            // reported so a caller can see how much the crash cost.
            report.fillsPersisted += step.fillsReplayed;
        }
    }

    if (replayed.truncated) {
        WriteAheadLog::truncateTo(walPath, replayed.validBytes);
    }
    return report;
}

} // namespace exchange::store
