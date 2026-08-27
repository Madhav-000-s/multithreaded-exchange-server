#include "net/frame_assembler.hpp"

#include "core/exceptions.hpp"
#include "core/wire.hpp"
#include "net/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace exchange::net {

void FrameAssembler::append(std::span<const std::byte> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::optional<Frame> FrameAssembler::next() {
    compact();

    // Not even a header yet: wait for more bytes rather than guessing.
    if (buffered() < kHeaderSize) {
        return std::nullopt;
    }

    const std::span<const std::byte> view(buffer_.data() + readPos_, buffered());
    wire::Reader header(view);
    const std::uint32_t length = header.u32();

    // The length is peer-controlled, so it is bounds-checked before it is
    // used for anything -- including before it is used to decide how much to
    // wait for. A four-byte header claiming 4 GiB must not become a 4 GiB
    // allocation, or even a 4 GiB expectation.
    if (length > maxBody_) {
        throw ProtocolError("frame body " + std::to_string(length) + " exceeds the " +
                            std::to_string(maxBody_) + " byte limit");
    }
    // len counts version and type, so anything below 2 cannot describe a real
    // message and would underflow the payload size computed below.
    if (length < 2) {
        throw ProtocolError("frame body " + std::to_string(length) + " is too short to be valid");
    }

    const std::size_t total = sizeof(std::uint32_t) + length;
    if (buffered() < total) {
        return std::nullopt; // header complete, body still in flight
    }

    const std::uint8_t version = header.u8();
    if (version != kProtocolVersion) {
        throw ProtocolError("unsupported protocol version " + std::to_string(version));
    }
    const auto type = static_cast<MessageType>(header.u8());

    Frame frame;
    frame.version = version;
    frame.type = type;
    frame.payload = view.subspan(kHeaderSize, length - 2);

    readPos_ += total;
    return frame;
}

void FrameAssembler::compact() {
    if (readPos_ == 0) {
        return;
    }
    // Amortised: only once the dead prefix is at least half the buffer, so a
    // stream of small frames does not memmove the tail on every message.
    if (readPos_ * 2 < buffer_.size()) {
        return;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(readPos_));
    readPos_ = 0;
}

} // namespace exchange::net
