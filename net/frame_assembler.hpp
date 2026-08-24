#pragma once

#include "net/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace exchange::net {

/// One complete message, still undecoded.
struct Frame {
    std::uint8_t version{};
    MessageType type{};

    /// A view into the assembler's buffer.
    ///
    /// **Valid only until the next call to `append` or `next` on that
    /// assembler.** Both can reallocate or compact the buffer. The caller
    /// decodes immediately, so a view is right here and a copy per frame would
    /// be pure waste -- but the lifetime has to be stated, because a dangling
    /// span is exactly as broken as a dangling pointer and looks safer.
    std::span<const std::byte> payload;
};

/// Turns a TCP byte stream back into messages.
///
/// TCP preserves order and delivers every byte exactly once, and guarantees
/// nothing else. A peer's single 40-byte write can arrive as one read of 40,
/// as forty reads of one byte, or as the tail of one order glued to the head
/// of the next. All three are normal, and a decoder that assumes one read is
/// one message works perfectly on loopback and fails under load -- which is
/// the worst way for it to fail, because the tests pass.
///
/// So bytes accumulate here until a length prefix says a whole message is
/// present. `next()` yields frames one at a time and reports absence rather
/// than blocking; the reactor calls it in a loop after each read.
class FrameAssembler {
public:
    explicit FrameAssembler(std::uint32_t maxBody = kMaxFrameBody) noexcept : maxBody_(maxBody) {}

    /// Adds freshly read bytes.
    void append(std::span<const std::byte> bytes);

    /// Extracts the next complete frame, or nullopt if one has not arrived.
    ///
    /// @throws ProtocolError if a header declares a body over the cap, or
    ///         names an unsupported version. Both mean the stream can no
    ///         longer be trusted, so the caller drops the session -- resyncing
    ///         is impossible when the framing itself is what went wrong.
    [[nodiscard]] std::optional<Frame> next();

    /// Bytes held but not yet forming a complete frame.
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - readPos_; }

    [[nodiscard]] std::uint32_t maxBody() const noexcept { return maxBody_; }

private:
    /// Drops the consumed prefix once it is worth the memmove.
    ///
    /// Without this the buffer grows without bound on a long-lived session
    /// even though the live data stays small -- a slow leak that only shows up
    /// after hours. Compacting on every frame would instead memmove the tail
    /// constantly, so it is amortised: only when the dead prefix is at least
    /// half the buffer.
    void compact();

    std::vector<std::byte> buffer_;
    std::size_t readPos_{0};
    std::uint32_t maxBody_;
};

} // namespace exchange::net
