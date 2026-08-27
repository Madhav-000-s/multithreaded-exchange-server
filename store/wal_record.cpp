#include "store/wal_record.hpp"

#include "core/command.hpp"
#include "core/order.hpp"
#include "core/types.hpp"
#include "core/wire.hpp"
#include "store/crc32.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace exchange::store {
namespace {

/// Distinguishes the concrete order type on disk. Independent of the network
/// protocol's OrderKind: the log is free to gain a type without a protocol
/// version bump, and vice versa.
enum class StoredOrderKind : std::uint8_t {
    Limit = 0,
    Market = 1,
    Iceberg = 2,
};

void writeOrder(wire::Writer& writer, const Order& order) {
    const std::optional<Price> resting = order.restingPrice();

    StoredOrderKind kind = StoredOrderKind::Market;
    if (resting.has_value()) {
        // An iceberg is a limit order that shows less than it holds, which is
        // exactly what displaySize() reports. Asking the object rather than
        // testing its type keeps the log writer out of the hierarchy.
        kind = order.displaySize() < order.remaining() ? StoredOrderKind::Iceberg
                                                       : StoredOrderKind::Limit;
    }

    writer.u8(static_cast<std::uint8_t>(kind));
    writer.u64(order.id());
    writer.u32(order.account());
    writer.u8(static_cast<std::uint8_t>(order.side()));
    writer.i64(resting.value_or(0));
    writer.u64(order.remaining());
    writer.u64(order.displaySize());
}

[[nodiscard]] std::unique_ptr<Order> readOrder(wire::Reader& reader) {
    const auto kind = static_cast<StoredOrderKind>(reader.u8());
    const OrderId id = reader.u64();
    const AccountId account = reader.u32();
    const auto side = static_cast<Side>(reader.u8());
    const Price price = reader.i64();
    const Quantity quantity = reader.u64();
    const Quantity display = reader.u64();

    switch (kind) {
    case StoredOrderKind::Limit:
        return std::make_unique<LimitOrder>(id, side, account, quantity, price);
    case StoredOrderKind::Market:
        return std::make_unique<MarketOrder>(id, side, account, quantity);
    case StoredOrderKind::Iceberg:
        return std::make_unique<IcebergOrder>(id, side, account, quantity, price, display);
    }
    return nullptr; // caller treats a null order as corruption
}

template <typename... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

template <typename... Handlers>
Overloaded(Handlers...) -> Overloaded<Handlers...>;

} // namespace

std::vector<std::byte> encodeRecord(Sequence sequence, const Command& command) {
    std::vector<std::byte> payload;
    payload.reserve(64);
    wire::Writer writer(payload);

    writer.u64(sequence);

    std::visit(Overloaded{
                   [&](const SubmitCommand& submit) {
                       writer.u8(static_cast<std::uint8_t>(RecordType::Submit));
                       writeOrder(writer, *submit.order);
                   },
                   [&](const CancelCommand& cancel) {
                       writer.u8(static_cast<std::uint8_t>(RecordType::Cancel));
                       writer.u64(cancel.id);
                   },
                   [&](const ModifyCommand& modify) {
                       writer.u8(static_cast<std::uint8_t>(RecordType::Modify));
                       writer.u64(modify.id);
                       writer.u64(modify.quantity);
                       writer.u8(modify.price.has_value() ? 1U : 0U);
                       writer.i64(modify.price.value_or(0));
                   },
               },
               command);

    std::vector<std::byte> record;
    record.reserve(kRecordHeaderSize + payload.size());
    wire::Writer header(record);
    header.u32(static_cast<std::uint32_t>(payload.size()));
    header.u32(crc32(payload));

    record.insert(record.end(), payload.begin(), payload.end());
    return record;
}

ReadResult decodeRecord(std::span<const std::byte> bytes) noexcept {
    ReadResult result;

    if (bytes.size() < kRecordHeaderSize) {
        result.status = ReadStatus::Incomplete;
        return result;
    }

    // The header is read by hand rather than through wire::Reader because that
    // throws on underrun, and a short read here is the normal end of a log
    // rather than an error.
    const auto readU32 = [bytes](std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8U * i);
        }
        return value;
    };

    const std::uint32_t length = readU32(0);
    const std::uint32_t expectedCrc = readU32(4);

    if (length == 0 || length > kMaxRecordPayload) {
        result.status = ReadStatus::Corrupt;
        return result;
    }
    if (bytes.size() < kRecordHeaderSize + length) {
        // The writer was killed between extending the file and finishing the
        // payload. Everything before this point is still good.
        result.status = ReadStatus::Incomplete;
        return result;
    }

    const std::span<const std::byte> payload = bytes.subspan(kRecordHeaderSize, length);
    if (crc32(payload) != expectedCrc) {
        // Structurally plausible and semantically garbage -- the case a length
        // check alone cannot catch.
        result.status = ReadStatus::Corrupt;
        return result;
    }

    try {
        wire::Reader reader(payload);
        WalRecord record;
        record.sequence = reader.u64();

        switch (static_cast<RecordType>(reader.u8())) {
        case RecordType::Submit: {
            std::unique_ptr<Order> order = readOrder(reader);
            if (order == nullptr) {
                result.status = ReadStatus::Corrupt;
                return result;
            }
            record.command = SubmitCommand{.order = std::move(order)};
            break;
        }
        case RecordType::Cancel:
            record.command = CancelCommand{.id = reader.u64()};
            break;
        case RecordType::Modify: {
            ModifyCommand modify;
            modify.id = reader.u64();
            modify.quantity = reader.u64();
            const bool hasPrice = reader.u8() != 0;
            const Price price = reader.i64();
            if (hasPrice) {
                modify.price = price;
            }
            record.command = modify;
            break;
        }
        default:
            result.status = ReadStatus::Corrupt;
            return result;
        }

        reader.requireFullyConsumed();

        result.status = ReadStatus::Ok;
        result.consumed = kRecordHeaderSize + length;
        result.record = std::move(record);
        return result;
    } catch (...) {
        // wire::Reader throws on underrun or trailing bytes. Either means the
        // payload does not match its declared length despite a valid checksum,
        // which is corruption by any other name.
        result.status = ReadStatus::Corrupt;
        return result;
    }
}

} // namespace exchange::store
