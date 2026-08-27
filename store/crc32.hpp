#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace exchange::store {

/// CRC-32 (IEEE 802.3, the zlib polynomial), table-driven.
///
/// Hand-rolled rather than pulling in zlib for thirty lines. The table is
/// built at compile time, so there is no initialisation order to get wrong and
/// no runtime cost to the first call.
///
/// **What it is for, and what it is not.** This detects a *torn* record: a
/// process killed mid-write leaves a tail that is structurally plausible --
/// the length prefix reads fine, the bytes after it are whatever the previous
/// occupant of that disk block happened to be -- and replaying it would inject
/// a fabricated order into a recovered book. A checksum makes that trailing
/// garbage detectable rather than convincing.
///
/// It is not a security measure. CRC-32 is trivial to forge deliberately; it
/// catches accident, not malice. The WAL is a local file written only by this
/// process, so accident is the whole threat model.
namespace detail {

[[nodiscard]] constexpr std::array<std::uint32_t, 256> makeCrcTable() noexcept {
    constexpr std::uint32_t kPolynomial = 0xEDB88320U;
    std::array<std::uint32_t, 256> table{};

    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = ((value & 1U) != 0U) ? ((value >> 1U) ^ kPolynomial) : (value >> 1U);
        }
        table[i] = value;
    }
    return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrcTable = makeCrcTable();

} // namespace detail

[[nodiscard]] inline std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte byte : bytes) {
        const std::uint8_t index =
            static_cast<std::uint8_t>((crc ^ std::to_integer<std::uint32_t>(byte)) & 0xFFU);
        crc = (crc >> 8U) ^ detail::kCrcTable[index];
    }
    return crc ^ 0xFFFFFFFFU;
}

} // namespace exchange::store
