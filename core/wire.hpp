#pragma once

#include "core/exceptions.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace exchange::wire {

/// Little-endian, field by field, with explicit shifts.
///
/// Lives in core rather than net because two components serialise: the wire
/// protocol and the write-ahead log. Both need exactly these guarantees --
/// fixed width, defined byte order, no alignment assumptions -- and a WAL that
/// had to depend on the network layer to write a number would have the
/// dependency graph backwards.
///
/// The tempting alternative is to `memcpy` a packed struct onto the wire. It is
/// wrong three separate ways, and every one of them is silent:
///
///   - **Padding.** Without `#pragma pack` the compiler inserts alignment
///     holes, so the layout is not what the field list suggests. With it,
///     every member access becomes potentially unaligned.
///   - **Endianness.** The bytes come out in host order. Two machines that
///     disagree parse each other's orders as different numbers rather than as
///     an error.
///   - **Alignment.** Reading a `std::uint32_t` out of an arbitrary offset in
///     a byte buffer by casting the pointer is undefined behaviour, and on
///     some architectures a SIGBUS. `-Wcast-align` was turned on in Phase 0
///     with exactly this code in mind.
///
/// Shifting one byte at a time has none of those problems: it is defined,
/// portable, alignment-free, and the compiler folds it into a single
/// instruction on any target where that is legal anyway.
///
/// Everything here operates on `std::byte`, not `char` or `unsigned char`.
/// A byte on the wire is not a character and has no arithmetic meaning; using
/// the type that says so keeps a stray `+` from compiling.

/// Sequential writer appending to a caller-owned buffer.
class Writer {
public:
    explicit Writer(std::vector<std::byte>& out) noexcept : out_(&out) {}

    void u8(std::uint8_t value) { out_->push_back(static_cast<std::byte>(value)); }

    void u16(std::uint16_t value) { emit(value, 2); }

    void u32(std::uint32_t value) { emit(value, 4); }

    void u64(std::uint64_t value) { emit(value, 8); }

    /// Two's complement, reinterpreted as unsigned for transport.
    ///
    /// C++20 guarantees signed integers are two's complement, so this
    /// conversion is value-preserving and reversible -- which it was not
    /// before C++20, where the round trip was implementation-defined.
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

private:
    void emit(std::uint64_t value, std::size_t bytes) {
        for (std::size_t i = 0; i < bytes; ++i) {
            out_->push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
        }
    }

    std::vector<std::byte>* out_;
};

/// Sequential reader over a frame payload.
///
/// **Every read is bounds-checked and throws `ProtocolError` on underrun.**
/// Not a precondition, not an assert. This data arrives from a socket, so it
/// is attacker-controlled by definition -- and `EXCHANGE_PRECONDITION`
/// compiles to `__builtin_unreachable()` in release builds, which would tell
/// the optimiser to assume something a remote peer decides. That is not an
/// optimisation, it is a vulnerability.
class Reader {
public:
    explicit Reader(std::span<const std::byte> in) noexcept : in_(in) {}

    [[nodiscard]] std::uint8_t u8() {
        require(1);
        const auto value = std::to_integer<std::uint8_t>(in_[pos_]);
        ++pos_;
        return value;
    }

    [[nodiscard]] std::uint16_t u16() { return static_cast<std::uint16_t>(consume(2)); }

    [[nodiscard]] std::uint32_t u32() { return static_cast<std::uint32_t>(consume(4)); }

    [[nodiscard]] std::uint64_t u64() { return consume(8); }

    [[nodiscard]] std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

    [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }

    [[nodiscard]] bool exhausted() const noexcept { return pos_ == in_.size(); }

    /// Rejects a payload with bytes left over.
    ///
    /// Trailing data means the sender and this decoder disagree about the
    /// message layout, which is exactly the situation where continuing is
    /// worse than stopping -- the fields already read may be misaligned too.
    void requireFullyConsumed() const {
        if (!exhausted()) {
            throw ProtocolError("trailing bytes in message payload");
        }
    }

private:
    void require(std::size_t bytes) const {
        if (remaining() < bytes) {
            throw ProtocolError("truncated message payload");
        }
    }

    [[nodiscard]] std::uint64_t consume(std::size_t bytes) {
        require(bytes);
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            value |= std::to_integer<std::uint64_t>(in_[pos_ + i]) << (8U * i);
        }
        pos_ += bytes;
        return value;
    }

    std::span<const std::byte> in_;
    std::size_t pos_{0};
};

} // namespace exchange::wire
