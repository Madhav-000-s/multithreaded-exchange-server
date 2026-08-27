// The wire codec and the frame assembler.
//
// Everything decoded here arrives from a socket, so the malformed-input cases
// are the point of the file rather than an afterthought. A codec that only
// works on input it produced itself is a codec that has never met a peer.

#include "core/exceptions.hpp"
#include "core/types.hpp"
#include "core/wire.hpp"
#include "net/frame_assembler.hpp"
#include "net/protocol.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace exchange::net {
namespace {

[[nodiscard]] std::span<const std::byte> payloadOf(const std::vector<std::byte>& frame) {
    return std::span<const std::byte>(frame).subspan(kHeaderSize);
}

[[nodiscard]] MessageType typeOf(const std::vector<std::byte>& frame) {
    return static_cast<MessageType>(std::to_integer<std::uint8_t>(frame[5]));
}

// ---------------------------------------------------------------------------
// Wire primitives
// ---------------------------------------------------------------------------

TEST(Wire, RoundTripsEveryWidth) {
    std::vector<std::byte> buffer;
    wire::Writer writer(buffer);
    writer.u8(0xAB);
    writer.u16(0xBEEF);
    writer.u32(0xDEADBEEF);
    writer.u64(0x0123456789ABCDEFULL);
    writer.i64(-42);

    wire::Reader reader(buffer);
    EXPECT_EQ(reader.u8(), 0xAB);
    EXPECT_EQ(reader.u16(), 0xBEEF);
    EXPECT_EQ(reader.u32(), 0xDEADBEEFU);
    EXPECT_EQ(reader.u64(), 0x0123456789ABCDEFULL);
    EXPECT_EQ(reader.i64(), -42);
    EXPECT_TRUE(reader.exhausted());
}

TEST(Wire, IsLittleEndianOnTheWire) {
    // Pinned explicitly, because "whatever this machine does" is not a
    // protocol. Two hosts that disagree would parse each other's orders as
    // different numbers rather than as an error.
    std::vector<std::byte> buffer;
    wire::Writer writer(buffer);
    writer.u32(0x01020304);

    ASSERT_EQ(buffer.size(), 4u);
    EXPECT_EQ(std::to_integer<int>(buffer[0]), 0x04);
    EXPECT_EQ(std::to_integer<int>(buffer[1]), 0x03);
    EXPECT_EQ(std::to_integer<int>(buffer[2]), 0x02);
    EXPECT_EQ(std::to_integer<int>(buffer[3]), 0x01);
}

TEST(Wire, NegativePricesSurviveTheRoundTrip) {
    // Not hypothetical: WTI settled at -$37 in April 2020, which is why Price
    // is signed. C++20 guarantees two's complement, so the unsigned transport
    // is value-preserving.
    std::vector<std::byte> buffer;
    wire::Writer writer(buffer);
    writer.i64(-3700);
    writer.i64(INT64_MIN);

    wire::Reader reader(buffer);
    EXPECT_EQ(reader.i64(), -3700);
    EXPECT_EQ(reader.i64(), INT64_MIN);
}

TEST(Wire, ReadingPastTheEndThrowsRatherThanAsserting) {
    // A precondition would be exactly wrong here: it compiles to
    // __builtin_unreachable() in release, handing the optimiser an assumption
    // a remote peer controls.
    const std::vector<std::byte> buffer(3);
    wire::Reader reader(buffer);

    EXPECT_THROW((void)reader.u64(), ProtocolError);
}

TEST(Wire, TrailingBytesAreRejected) {
    std::vector<std::byte> buffer;
    wire::Writer writer(buffer);
    writer.u32(1);
    writer.u8(0);

    wire::Reader reader(buffer);
    EXPECT_EQ(reader.u32(), 1u);
    EXPECT_THROW(reader.requireFullyConsumed(), ProtocolError);
}

// ---------------------------------------------------------------------------
// Message codec
// ---------------------------------------------------------------------------

TEST(Protocol, NewOrderRoundTrips) {
    const NewOrderMessage sent{.orderId = 42,
                               .account = 7,
                               .side = Side::Buy,
                               .kind = OrderKind::Limit,
                               .price = -1250,
                               .quantity = 900,
                               .displayQuantity = 0};

    const std::vector<std::byte> frame = encode(sent);
    const auto decoded = decodeClientMessage(typeOf(frame), payloadOf(frame));

    ASSERT_TRUE(std::holds_alternative<NewOrderMessage>(decoded));
    EXPECT_EQ(std::get<NewOrderMessage>(decoded), sent);
}

TEST(Protocol, IcebergCarriesItsDisplaySize) {
    const NewOrderMessage sent{.orderId = 1,
                               .account = 2,
                               .side = Side::Sell,
                               .kind = OrderKind::Iceberg,
                               .price = 100,
                               .quantity = 500,
                               .displayQuantity = 25};

    const std::vector<std::byte> frame = encode(sent);
    const auto decoded = decodeClientMessage(typeOf(frame), payloadOf(frame));

    EXPECT_EQ(std::get<NewOrderMessage>(decoded).displayQuantity, 25u);
}

TEST(Protocol, CancelRoundTrips) {
    const CancelMessage sent{.orderId = 99};

    const std::vector<std::byte> frame = encode(sent);
    const auto decoded = decodeClientMessage(typeOf(frame), payloadOf(frame));

    EXPECT_EQ(std::get<CancelMessage>(decoded), sent);
}

TEST(Protocol, ModifyDistinguishesAbsentFromZeroPrice) {
    // The reason price presence is a separate byte: with negative prices
    // legal, no value can be reserved as a "no price" sentinel.
    const ModifyMessage withPrice{.orderId = 5, .quantity = 10, .price = 0};
    const ModifyMessage withoutPrice{.orderId = 5, .quantity = 10, .price = std::nullopt};

    const std::vector<std::byte> a = encode(withPrice);
    const std::vector<std::byte> b = encode(withoutPrice);

    EXPECT_EQ(std::get<ModifyMessage>(decodeClientMessage(typeOf(a), payloadOf(a))), withPrice);
    EXPECT_EQ(std::get<ModifyMessage>(decodeClientMessage(typeOf(b), payloadOf(b))), withoutPrice);
    EXPECT_NE(a, b) << "a price of zero and no price must differ on the wire";
}

TEST(Protocol, ServerMessagesRoundTrip) {
    const AcceptedMessage accepted{.orderId = 1, .filledQuantity = 4, .restingQuantity = 6};
    const RejectedMessage rejected{.orderId = 2, .reason = RejectReason::InvalidOrder};
    const FillMessage fill{
        .orderId = 3, .counterpartyOrderId = 4, .price = 250, .quantity = 10, .side = Side::Sell};

    const std::vector<std::byte> a = encode(accepted);
    const std::vector<std::byte> r = encode(rejected);
    const std::vector<std::byte> f = encode(fill);

    EXPECT_EQ(std::get<AcceptedMessage>(decodeServerMessage(typeOf(a), payloadOf(a))), accepted);
    EXPECT_EQ(std::get<RejectedMessage>(decodeServerMessage(typeOf(r), payloadOf(r))), rejected);
    EXPECT_EQ(std::get<FillMessage>(decodeServerMessage(typeOf(f), payloadOf(f))), fill);
}

TEST(Protocol, HeaderLengthCoversVersionTypeAndPayload) {
    const std::vector<std::byte> frame = encode(CancelMessage{.orderId = 1});

    wire::Reader reader(frame);
    const std::uint32_t length = reader.u32();

    EXPECT_EQ(length + sizeof(std::uint32_t), frame.size());
    EXPECT_EQ(reader.u8(), kProtocolVersion);
    EXPECT_EQ(reader.u8(), static_cast<std::uint8_t>(MessageType::Cancel));
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

TEST(Protocol, UnknownMessageTypeIsRejected) {
    const std::vector<std::byte> payload(8);

    EXPECT_THROW((void)decodeClientMessage(static_cast<MessageType>(200), payload), ProtocolError);
}

TEST(Protocol, ServerMessageOnTheInboundPathIsRejected) {
    // A client must not be able to inject a fill report.
    const std::vector<std::byte> frame = encode(FillMessage{});

    EXPECT_THROW((void)decodeClientMessage(MessageType::Fill, payloadOf(frame)), ProtocolError);
}

TEST(Protocol, OutOfRangeEnumIsRejectedRatherThanCast) {
    // A raw cast would produce a Side outside its enumerators, and every
    // switch on it would silently take the default branch.
    NewOrderMessage sent{};
    sent.side = Side::Buy;
    std::vector<std::byte> frame = encode(sent);

    // Byte layout: header(6) + orderId(8) + account(4), then side.
    frame[kHeaderSize + 12] = static_cast<std::byte>(9);

    EXPECT_THROW((void)decodeClientMessage(MessageType::NewOrder, payloadOf(frame)), ProtocolError);
}

TEST(Protocol, TruncatedPayloadIsRejected) {
    std::vector<std::byte> frame = encode(CancelMessage{.orderId = 1});
    frame.pop_back();

    EXPECT_THROW((void)decodeClientMessage(MessageType::Cancel, payloadOf(frame)), ProtocolError);
}

TEST(Protocol, OverlongPayloadIsRejected) {
    std::vector<std::byte> frame = encode(CancelMessage{.orderId = 1});
    frame.push_back(std::byte{0});

    EXPECT_THROW((void)decodeClientMessage(MessageType::Cancel, payloadOf(frame)), ProtocolError);
}

// ---------------------------------------------------------------------------
// Framing -- the part TCP makes necessary
// ---------------------------------------------------------------------------

TEST(FrameAssembler, YieldsNothingUntilAFrameIsComplete) {
    FrameAssembler assembler;

    EXPECT_FALSE(assembler.next().has_value());
}

TEST(FrameAssembler, ReassemblesAMessageDeliveredOneByteAtATime) {
    // The case that separates a working decoder from one that only works on
    // loopback: a single write arriving as many reads.
    const std::vector<std::byte> frame = encode(CancelMessage{.orderId = 7});
    FrameAssembler assembler;

    for (std::size_t i = 0; i + 1 < frame.size(); ++i) {
        assembler.append(std::span<const std::byte>(frame).subspan(i, 1));
        EXPECT_FALSE(assembler.next().has_value()) << "incomplete at byte " << i;
    }
    assembler.append(std::span<const std::byte>(frame).subspan(frame.size() - 1, 1));

    const std::optional<Frame> assembled = assembler.next();
    ASSERT_TRUE(assembled.has_value());
    EXPECT_EQ(assembled->type, MessageType::Cancel);
    EXPECT_EQ(
        std::get<CancelMessage>(decodeClientMessage(assembled->type, assembled->payload)).orderId,
        7u);
}

TEST(FrameAssembler, SplitsSeveralMessagesCoalescedIntoOneRead) {
    // The mirror case: several writes arriving as one read. Both happen, and
    // a decoder that assumes one read is one message fails on both.
    std::vector<std::byte> stream;
    for (OrderId id = 1; id <= 4; ++id) {
        const std::vector<std::byte> frame = encode(CancelMessage{.orderId = id});
        stream.insert(stream.end(), frame.begin(), frame.end());
    }

    FrameAssembler assembler;
    assembler.append(stream);

    std::vector<OrderId> received;
    while (const std::optional<Frame> frame = assembler.next()) {
        received.push_back(
            std::get<CancelMessage>(decodeClientMessage(frame->type, frame->payload)).orderId);
    }

    EXPECT_EQ(received, (std::vector<OrderId>{1, 2, 3, 4}));
}

TEST(FrameAssembler, HandlesAMessageBoundaryMidRead) {
    // One and a half messages, then the rest -- the realistic case.
    const std::vector<std::byte> first = encode(CancelMessage{.orderId = 1});
    const std::vector<std::byte> second = encode(CancelMessage{.orderId = 2});

    std::vector<std::byte> stream = first;
    stream.insert(stream.end(), second.begin(), second.begin() + 4);

    FrameAssembler assembler;
    assembler.append(stream);

    ASSERT_TRUE(assembler.next().has_value());
    EXPECT_FALSE(assembler.next().has_value()) << "the second message is still partial";

    assembler.append(std::span<const std::byte>(second).subspan(4));
    ASSERT_TRUE(assembler.next().has_value());
}

TEST(FrameAssembler, RejectsALengthOverTheCap) {
    // The security-critical case. A four-byte header claiming a huge body must
    // not become a huge allocation -- it costs an attacker nothing to send.
    FrameAssembler assembler(64);

    std::vector<std::byte> hostile;
    wire::Writer writer(hostile);
    writer.u32(0xFFFFFFFFU);
    writer.u8(kProtocolVersion);
    writer.u8(static_cast<std::uint8_t>(MessageType::Cancel));

    assembler.append(hostile);
    EXPECT_THROW((void)assembler.next(), ProtocolError);
}

TEST(FrameAssembler, RejectsALengthTooSmallToBeValid) {
    // len counts version and type, so anything under 2 would underflow the
    // payload size computed from it.
    FrameAssembler assembler;

    std::vector<std::byte> malformed;
    wire::Writer writer(malformed);
    writer.u32(1);
    writer.u8(kProtocolVersion);
    writer.u8(0);
    writer.u8(0);

    assembler.append(malformed);
    EXPECT_THROW((void)assembler.next(), ProtocolError);
}

TEST(FrameAssembler, RejectsAnUnsupportedVersion) {
    std::vector<std::byte> frame = encode(CancelMessage{.orderId = 1});
    frame[4] = static_cast<std::byte>(kProtocolVersion + 1);

    FrameAssembler assembler;
    assembler.append(frame);

    EXPECT_THROW((void)assembler.next(), ProtocolError);
}

TEST(FrameAssembler, DoesNotGrowWithoutBoundOnALongStream) {
    // Without compaction the buffer accumulates every byte ever received, even
    // though the live data stays tiny -- a leak that only shows after hours.
    FrameAssembler assembler;

    for (OrderId id = 1; id <= 2000; ++id) {
        assembler.append(encode(CancelMessage{.orderId = id}));
        ASSERT_TRUE(assembler.next().has_value());
    }

    EXPECT_EQ(assembler.buffered(), 0u);
}

} // namespace
} // namespace exchange::net
