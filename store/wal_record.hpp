#pragma once

#include "core/command.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace exchange::store {

/// On-disk record layout:
///
/// ```
/// +------------+-------------+---------------------+
/// | len (u32)  | crc32 (u32) | payload (len bytes) |
/// +------------+-------------+---------------------+
/// ```
///
/// The payload carries a sequence number, a command discriminant and the
/// command's fields. The CRC covers the payload only -- the header describes
/// where the payload is, and a corrupt header is caught by the length being
/// implausible rather than by a checksum over itself.
///
/// **Why not reuse the network frame format.** They look similar and answer
/// different questions. The wire format is a protocol negotiated with an
/// untrusted peer and versioned for compatibility; this is a private file
/// written and read by one process, where the questions are torn writes and
/// replay ordering. Sharing the format would couple a protocol change to a
/// storage migration, which is exactly the coupling that makes both hard to
/// change. They share the *primitives* -- core/wire.hpp -- and nothing else.
inline constexpr std::size_t kRecordHeaderSize = 8; // u32 length + u32 crc

/// Largest payload accepted on replay. A length beyond this in a WAL means the
/// file is damaged, not that a very large order was written.
inline constexpr std::uint32_t kMaxRecordPayload = 4096;

enum class RecordType : std::uint8_t {
    Submit = 1,
    Cancel = 2,
    Modify = 3,
};

/// A command plus the position it occupied in the log.
struct WalRecord {
    Sequence sequence{0};
    Command command;
};

/// Serialises a complete record, header included.
[[nodiscard]] std::vector<std::byte> encodeRecord(Sequence sequence, const Command& command);

/// Outcome of trying to read one record from a buffer.
enum class ReadStatus {
    Ok,
    /// Not enough bytes yet -- for a file, this means the log ends here.
    Incomplete,
    /// Structurally impossible, or the checksum disagrees. Replay stops.
    Corrupt,
};

struct ReadResult {
    ReadStatus status{ReadStatus::Incomplete};
    std::optional<WalRecord> record;
    /// Bytes consumed when status is Ok.
    std::size_t consumed{0};
};

/// Decodes one record from the front of `bytes`.
///
/// Never throws on damaged input: a truncated or corrupt tail is the *expected*
/// state of a log whose writer was killed, so it is reported as a status the
/// caller acts on rather than an exception it has to treat as exceptional.
[[nodiscard]] ReadResult decodeRecord(std::span<const std::byte> bytes) noexcept;

} // namespace exchange::store
