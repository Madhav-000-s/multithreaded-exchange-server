#pragma once

#include "core/command.hpp"
#include "core/command_log.hpp"
#include "core/types.hpp"
#include "store/wal_record.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace exchange::store {

/// When the log is forced to stable storage.
///
/// **The central durability/throughput trade in the system**, which is why it
/// is configurable rather than chosen once and buried.
///
/// A plain `write()` only copies into the kernel page cache. The data survives
/// the process dying -- the kernel still has it and will write it out -- but
/// not the machine losing power. Only `fsync`/`fdatasync` forces it to the
/// device, and that costs a device round trip.
enum class SyncPolicy {
    /// fdatasync after every record. Nothing is acknowledged that is not on
    /// the platter. Slowest by a wide margin, and the only setting under which
    /// "we told the client it was done" is unconditionally true.
    Always,

    /// fdatasync every N records or every T milliseconds, whichever comes
    /// first. The loss window is bounded and stated rather than unknown, which
    /// is what makes this the setting most real systems actually run.
    Batch,

    /// Never force. Survives `kill -9` -- the page cache is the kernel's, not
    /// the process's -- but not power loss. Fast, and honest about what it
    /// does not promise.
    Never,
};

struct WalConfig {
    std::string path;
    SyncPolicy policy{SyncPolicy::Batch};

    /// Batch policy: force after this many records.
    std::size_t batchRecords{64};

    /// Batch policy: force at least this often.
    std::chrono::milliseconds batchInterval{10};
};

/// Append-only write-ahead log.
///
/// **The invariant: the record is durable before the mutation is visible.**
/// The engine appends, the append returns, and only then is the book changed.
/// A crash between the two loses nothing that was acknowledged -- recovery
/// replays the record and reaches the state the client was told about.
///
/// Doing it the other way round -- mutate, then log -- is write-behind, and it
/// loses exactly the orders that were acknowledged just before the crash. The
/// ordering is the whole point of the name.
///
/// Single-writer by construction: the engine thread owns this, as it owns the
/// book. No locking, for the same reason.
class WriteAheadLog final : public ICommandLog {
public:
    /// Opens or creates the log, positioned at the end.
    /// @throws StorageError if the file cannot be opened.
    explicit WriteAheadLog(WalConfig config);

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;
    WriteAheadLog(WriteAheadLog&&) = delete;
    WriteAheadLog& operator=(WriteAheadLog&&) = delete;

    ~WriteAheadLog() override;

    /// Appends a command and applies the sync policy.
    ///
    /// @return the sequence number assigned.
    /// @throws StorageError if the write or the sync fails. The caller must
    ///         *not* apply the command in that case -- an unlogged mutation is
    ///         precisely the divergence the log exists to prevent.
    Sequence append(const Command& command) override;

    /// Forces everything written so far to stable storage, whatever the
    /// policy. Called on clean shutdown so a graceful stop loses nothing even
    /// under Batch or Never.
    void flush() override;

    [[nodiscard]] Sequence lastSequence() const noexcept { return nextSequence_ - 1; }

    [[nodiscard]] std::size_t recordsWritten() const noexcept { return recordsWritten_; }

    /// Times fdatasync was actually called. The observable difference between
    /// the policies, and what the benchmark measures.
    [[nodiscard]] std::size_t syncCount() const noexcept { return syncCount_; }

    [[nodiscard]] const WalConfig& config() const noexcept { return config_; }

    /// Reads every intact record from a log file, stopping at the first
    /// incomplete or corrupt one.
    ///
    /// Stopping rather than skipping is deliberate. A gap in a command log is
    /// not a missing row, it is a different history: replaying what follows a
    /// hole produces a book that never existed. The truncation point is
    /// reported so the caller can trim the file to it.
    struct ReplayResult {
        std::vector<WalRecord> records;
        /// Byte offset of the first byte not part of an intact record.
        std::size_t validBytes{0};
        /// True if the tail was damaged rather than merely absent.
        bool truncated{false};
    };

    [[nodiscard]] static ReplayResult replay(const std::string& path);

    /// Discards everything after `validBytes`, so the next append starts from
    /// a clean boundary rather than after garbage.
    static void truncateTo(const std::string& path, std::size_t validBytes);

private:
    void syncNow();
    [[nodiscard]] bool shouldSync() const;

    WalConfig config_;
    int fd_{-1};
    Sequence nextSequence_{1};
    std::size_t recordsWritten_{0};
    std::size_t syncCount_{0};
    std::size_t sinceSync_{0};
    std::chrono::steady_clock::time_point lastSync_{};
};

} // namespace exchange::store
