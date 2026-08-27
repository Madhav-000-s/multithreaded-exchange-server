#include "store/write_ahead_log.hpp"

#include "core/exceptions.hpp"
#include "store/wal_record.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace exchange::store {
namespace {

[[nodiscard]] std::string describe(const char* what, const std::string& path) {
    return std::string(what) + " '" + path + "': " + std::strerror(errno);
}

/// Writes the whole buffer, retrying short writes.
///
/// A single write() is permitted to transfer fewer bytes than asked, and
/// treating a short write as complete is how a log quietly grows records that
/// are missing their tail. That the tail would then fail its checksum is the
/// backstop, not the plan.
void writeFully(int fd, std::span<const std::byte> bytes, const std::string& path) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t wrote = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (wrote > 0) {
            written += static_cast<std::size_t>(wrote);
            continue;
        }
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        throw StorageError(describe("write to WAL", path));
    }
}

} // namespace

WriteAheadLog::WriteAheadLog(WalConfig config) : config_(std::move(config)) {
    // O_APPEND makes every write atomic with respect to the file offset, so
    // even a concurrent writer could not interleave a partial record. The
    // engine is the only writer, but the flag costs nothing and removes a
    // whole class of question.
    fd_ = ::open(config_.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        throw StorageError(describe("open WAL", config_.path));
    }

    // Continue the sequence rather than restart it, so numbers stay unique
    // across a restart and a recovered log reads as one history.
    const ReplayResult existing = replay(config_.path);
    if (!existing.records.empty()) {
        nextSequence_ = existing.records.back().sequence + 1;
    }

    lastSync_ = std::chrono::steady_clock::now();
}

WriteAheadLog::~WriteAheadLog() {
    if (fd_ >= 0) {
        // Best effort: a destructor must not throw, and there is nobody left
        // to tell. A caller that needs the guarantee calls flush() first.
        (void)::fdatasync(fd_);
        ::close(fd_);
    }
}

Sequence WriteAheadLog::append(const Command& command) {
    const Sequence sequence = nextSequence_;
    const std::vector<std::byte> record = encodeRecord(sequence, command);

    writeFully(fd_, record, config_.path);

    ++nextSequence_;
    ++recordsWritten_;
    ++sinceSync_;

    if (shouldSync()) {
        syncNow();
    }
    return sequence;
}

bool WriteAheadLog::shouldSync() const {
    switch (config_.policy) {
    case SyncPolicy::Always:
        return true;
    case SyncPolicy::Never:
        return false;
    case SyncPolicy::Batch:
        if (sinceSync_ >= config_.batchRecords) {
            return true;
        }
        return std::chrono::steady_clock::now() - lastSync_ >= config_.batchInterval;
    }
    return false;
}

void WriteAheadLog::syncNow() {
    // fdatasync rather than fsync: fsync also forces the inode's metadata --
    // mtime, atime -- which is a second write to a different part of the disk
    // for information nothing here reads. fdatasync still forces the size
    // update, which is the only metadata an append actually depends on.
    if (::fdatasync(fd_) != 0) {
        throw StorageError(describe("fdatasync WAL", config_.path));
    }
    ++syncCount_;
    sinceSync_ = 0;
    lastSync_ = std::chrono::steady_clock::now();
}

void WriteAheadLog::flush() {
    if (fd_ >= 0) {
        syncNow();
    }
}

WriteAheadLog::ReplayResult WriteAheadLog::replay(const std::string& path) {
    ReplayResult result;

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return result; // no log yet is a normal first start
        }
        throw StorageError(describe("open WAL for replay", path));
    }

    std::vector<std::byte> contents;
    std::vector<std::byte> chunk(64 * 1024);
    while (true) {
        const ssize_t got = ::read(fd, chunk.data(), chunk.size());
        if (got > 0) {
            contents.insert(contents.end(), chunk.begin(),
                            chunk.begin() + static_cast<std::ptrdiff_t>(got));
            continue;
        }
        if (got == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        ::close(fd);
        throw StorageError(describe("read WAL", path));
    }
    ::close(fd);

    std::size_t offset = 0;
    while (offset < contents.size()) {
        ReadResult read = decodeRecord(std::span<const std::byte>(contents).subspan(offset));

        if (read.status == ReadStatus::Ok) {
            offset += read.consumed;
            result.records.push_back(std::move(*read.record));
            continue;
        }

        // Either an incomplete tail (killed mid-write) or a corrupt one
        // (killed after the length was extended but before the payload
        // landed). Both mean the log ends here: replaying past a hole would
        // produce a book that never existed.
        result.truncated = true;
        break;
    }

    result.validBytes = offset;
    return result;
}

void WriteAheadLog::truncateTo(const std::string& path, std::size_t validBytes) {
    if (::truncate(path.c_str(), static_cast<off_t>(validBytes)) != 0) {
        throw StorageError(describe("truncate WAL", path));
    }
}

} // namespace exchange::store
