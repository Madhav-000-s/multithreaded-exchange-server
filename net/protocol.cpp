#include "net/protocol.hpp"

#include "core/exceptions.hpp"
#include "core/types.hpp"
#include "core/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace exchange::net {
namespace {

/// Writes the header with a placeholder length, returning the buffer for the
/// body to be appended to. The length is patched once the body is known --
/// the alternative, encoding the body into a scratch buffer first and then
/// copying, costs an extra allocation and copy per message for no benefit.
[[nodiscard]] std::vector<std::byte> beginFrame(MessageType type) {
    std::vector<std::byte> frame;
    frame.reserve(kHeaderSize + 48);

    wire::Writer writer(frame);
    writer.u32(0); // patched by finishFrame
    writer.u8(kProtocolVersion);
    writer.u8(static_cast<std::uint8_t>(type));
    return frame;
}

void finishFrame(std::vector<std::byte>& frame) {
    // len covers version + type + payload, i.e. everything after the u32.
    const std::size_t body = frame.size() - sizeof(std::uint32_t);
    const auto length = static_cast<std::uint32_t>(body);

    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        frame[i] = static_cast<std::byte>((length >> (8U * i)) & 0xFFU);
    }
}

/// Validates an enum arriving from the wire.
///
/// A raw cast from a peer-supplied byte to an enum class produces a value
/// outside the enumerators, and every subsequent switch on it silently takes
/// the default branch. Range-checking at the boundary is what keeps an
/// invalid value from becoming a valid-looking one.
[[nodiscard]] Side decodeSide(std::uint8_t raw) {
    if (raw > static_cast<std::uint8_t>(Side::Sell)) {
        throw ProtocolError("side out of range: " + std::to_string(raw));
    }
    return static_cast<Side>(raw);
}

[[nodiscard]] OrderKind decodeKind(std::uint8_t raw) {
    if (raw > static_cast<std::uint8_t>(OrderKind::Iceberg)) {
        throw ProtocolError("order kind out of range: " + std::to_string(raw));
    }
    return static_cast<OrderKind>(raw);
}

[[nodiscard]] RejectReason decodeReason(std::uint8_t raw) {
    if (raw > static_cast<std::uint8_t>(RejectReason::Overloaded)) {
        throw ProtocolError("reject reason out of range: " + std::to_string(raw));
    }
    return static_cast<RejectReason>(raw);
}

} // namespace

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

std::vector<std::byte> encode(const NewOrderMessage& message) {
    std::vector<std::byte> frame = beginFrame(MessageType::NewOrder);
    wire::Writer writer(frame);
    writer.u64(message.orderId);
    writer.u32(message.account);
    writer.u8(static_cast<std::uint8_t>(message.side));
    writer.u8(static_cast<std::uint8_t>(message.kind));
    writer.i64(message.price);
    writer.u64(message.quantity);
    writer.u64(message.displayQuantity);
    finishFrame(frame);
    return frame;
}

std::vector<std::byte> encode(const CancelMessage& message) {
    std::vector<std::byte> frame = beginFrame(MessageType::Cancel);
    wire::Writer writer(frame);
    writer.u64(message.orderId);
    finishFrame(frame);
    return frame;
}

std::vector<std::byte> encode(const ModifyMessage& message) {
    std::vector<std::byte> frame = beginFrame(MessageType::Modify);
    wire::Writer writer(frame);
    writer.u64(message.orderId);
    writer.u64(message.quantity);
    writer.u8(message.price.has_value() ? 1U : 0U);
    writer.i64(message.price.value_or(0));
    finishFrame(frame);
    return frame;
}

std::vector<std::byte> encode(const AcceptedMessage& message) {
    std::vector<std::byte> frame = beginFrame(MessageType::Accepted);
    wire::Writer writer(frame);
    writer.u64(message.orderId);
    writer.u64(message.filledQuantity);
    writer.u64(message.restingQuantity);
    finishFrame(frame);
    return frame;
}

std::vector<std::byte> encode(const RejectedMessage& message) {
    std::vector<std::byte> frame = beginFrame(MessageType::Rejected);
    wire::Writer writer(frame);
    writer.u64(message.orderId);
    writer.u8(static_cast<std::uint8_t>(message.reason));
    finishFrame(frame);
    return frame;
}

std::vector<std::byte> encode(const FillMessage& message) {
    std::vector<std::byte> frame = beginFrame(MessageType::Fill);
    wire::Writer writer(frame);
    writer.u64(message.orderId);
    writer.u64(message.counterpartyOrderId);
    writer.i64(message.price);
    writer.u64(message.quantity);
    writer.u8(static_cast<std::uint8_t>(message.side));
    finishFrame(frame);
    return frame;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

ClientMessage decodeClientMessage(MessageType type, std::span<const std::byte> payload) {
    wire::Reader reader(payload);

    switch (type) {
    case MessageType::NewOrder: {
        NewOrderMessage message;
        message.orderId = reader.u64();
        message.account = reader.u32();
        message.side = decodeSide(reader.u8());
        message.kind = decodeKind(reader.u8());
        message.price = reader.i64();
        message.quantity = reader.u64();
        message.displayQuantity = reader.u64();
        reader.requireFullyConsumed();
        return message;
    }
    case MessageType::Cancel: {
        CancelMessage message;
        message.orderId = reader.u64();
        reader.requireFullyConsumed();
        return message;
    }
    case MessageType::Modify: {
        ModifyMessage message;
        message.orderId = reader.u64();
        message.quantity = reader.u64();
        const bool hasPrice = reader.u8() != 0;
        const Price price = reader.i64();
        if (hasPrice) {
            message.price = price;
        }
        reader.requireFullyConsumed();
        return message;
    }
    case MessageType::Accepted:
    case MessageType::Rejected:
    case MessageType::Fill:
        throw ProtocolError("server message received on the inbound path");
    }

    throw ProtocolError("unknown message type: " + std::to_string(static_cast<std::uint8_t>(type)));
}

ServerMessage decodeServerMessage(MessageType type, std::span<const std::byte> payload) {
    wire::Reader reader(payload);

    switch (type) {
    case MessageType::Accepted: {
        AcceptedMessage message;
        message.orderId = reader.u64();
        message.filledQuantity = reader.u64();
        message.restingQuantity = reader.u64();
        reader.requireFullyConsumed();
        return message;
    }
    case MessageType::Rejected: {
        RejectedMessage message;
        message.orderId = reader.u64();
        message.reason = decodeReason(reader.u8());
        reader.requireFullyConsumed();
        return message;
    }
    case MessageType::Fill: {
        FillMessage message;
        message.orderId = reader.u64();
        message.counterpartyOrderId = reader.u64();
        message.price = reader.i64();
        message.quantity = reader.u64();
        message.side = decodeSide(reader.u8());
        reader.requireFullyConsumed();
        return message;
    }
    case MessageType::NewOrder:
    case MessageType::Cancel:
    case MessageType::Modify:
        throw ProtocolError("client message received on the outbound path");
    }

    throw ProtocolError("unknown message type: " + std::to_string(static_cast<std::uint8_t>(type)));
}

} // namespace exchange::net
