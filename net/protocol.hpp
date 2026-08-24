#pragma once

#include "core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace exchange::net {

/// Wire format, little-endian throughout:
///
/// ```
/// +------------+---------+----------+-------------------+
/// | len (u32)  | ver(u8) | type(u8) | payload (len-2 B) |
/// +------------+---------+----------+-------------------+
/// ```
///
/// `len` counts the version and type bytes plus the payload, so the total
/// bytes on the wire are `len + 4`.
///
/// **Why frame at all.** TCP is a byte stream, not a message stream. A single
/// `write()` of 40 bytes can arrive as one read of 40, forty reads of 1, or
/// half of one order followed by all of the next. Nothing in TCP preserves
/// message boundaries, so the application has to carry them -- by length
/// prefix, by delimiter, or by fixed size. A length prefix wins here because
/// binary payloads can contain any byte value (so no delimiter is safe) and
/// messages differ in size.
///
/// **Why the length comes first.** It lets the reader know how much to wait
/// for before it has to understand anything else, which is what makes partial
/// reads cheap to accumulate.
///
/// Hand-rolled rather than protobuf or FlatBuffers: the framing discussion is
/// the point of the exercise, and a generated codec would hide exactly the
/// decisions worth being able to defend.
inline constexpr std::uint8_t kProtocolVersion = 1;

/// u32 length + u8 version + u8 type.
inline constexpr std::size_t kHeaderSize = 6;

/// Largest `len` accepted, and therefore the largest buffer a peer can make
/// this process allocate.
///
/// **This bound is the single most security-critical constant in the file.**
/// The length prefix is attacker-controlled: without a cap, a four-byte header
/// claiming 4 GiB is a one-packet denial of service, and it costs nothing to
/// send. Every real framing protocol has this limit, and the ones that
/// discovered they needed it did so the hard way.
inline constexpr std::uint32_t kMaxFrameBody = 4096;

enum class MessageType : std::uint8_t {
    // Client to server.
    NewOrder = 1,
    Cancel = 2,
    Modify = 3,

    // Server to client.
    Accepted = 16,
    Rejected = 17,
    Fill = 18,
};

enum class OrderKind : std::uint8_t {
    Limit = 0,
    Market = 1,
    Iceberg = 2,
};

enum class RejectReason : std::uint8_t {
    Unknown = 0,
    InvalidOrder = 1,
    UnknownOrder = 2,
    SelfMatchBlocked = 3,
    Overloaded = 4,
};

// ---------------------------------------------------------------------------
// Client to server
// ---------------------------------------------------------------------------

struct NewOrderMessage {
    OrderId orderId{};
    AccountId account{};
    Side side{};
    OrderKind kind{};
    Price price{};
    Quantity quantity{};
    /// Only meaningful for Iceberg. Sent regardless so the layout is fixed
    /// width, which keeps the decoder branch-free and the frame size
    /// predictable.
    Quantity displayQuantity{};

    [[nodiscard]] friend bool operator==(const NewOrderMessage&, const NewOrderMessage&) = default;
};

struct CancelMessage {
    OrderId orderId{};

    [[nodiscard]] friend bool operator==(const CancelMessage&, const CancelMessage&) = default;
};

struct ModifyMessage {
    OrderId orderId{};
    Quantity quantity{};
    /// Encoded as a presence byte plus the value, rather than by sending a
    /// sentinel price. There is no price value that can be reserved as "no
    /// price" when negative prices are legal.
    std::optional<Price> price;

    [[nodiscard]] friend bool operator==(const ModifyMessage&, const ModifyMessage&) = default;
};

using ClientMessage = std::variant<NewOrderMessage, CancelMessage, ModifyMessage>;

// ---------------------------------------------------------------------------
// Server to client
// ---------------------------------------------------------------------------

struct AcceptedMessage {
    OrderId orderId{};
    Quantity filledQuantity{};
    Quantity restingQuantity{};

    [[nodiscard]] friend bool operator==(const AcceptedMessage&, const AcceptedMessage&) = default;
};

struct RejectedMessage {
    OrderId orderId{};
    RejectReason reason{RejectReason::Unknown};

    [[nodiscard]] friend bool operator==(const RejectedMessage&, const RejectedMessage&) = default;
};

struct FillMessage {
    OrderId orderId{};
    OrderId counterpartyOrderId{};
    Price price{};
    Quantity quantity{};
    Side side{};

    [[nodiscard]] friend bool operator==(const FillMessage&, const FillMessage&) = default;
};

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

/// Serialises a complete frame, header included.
[[nodiscard]] std::vector<std::byte> encode(const NewOrderMessage& message);
[[nodiscard]] std::vector<std::byte> encode(const CancelMessage& message);
[[nodiscard]] std::vector<std::byte> encode(const ModifyMessage& message);
[[nodiscard]] std::vector<std::byte> encode(const AcceptedMessage& message);
[[nodiscard]] std::vector<std::byte> encode(const RejectedMessage& message);
[[nodiscard]] std::vector<std::byte> encode(const FillMessage& message);

/// Decodes one client message from a frame's type byte and payload.
///
/// @throws ProtocolError on an unknown type, a truncated payload, trailing
///         bytes, or a field outside its permitted range.
[[nodiscard]] ClientMessage decodeClientMessage(MessageType type,
                                                std::span<const std::byte> payload);

/// Decodes one server message. Used by the test client and by anything
/// consuming the feed.
using ServerMessage = std::variant<AcceptedMessage, RejectedMessage, FillMessage>;

[[nodiscard]] ServerMessage decodeServerMessage(MessageType type,
                                                std::span<const std::byte> payload);

} // namespace exchange::net
