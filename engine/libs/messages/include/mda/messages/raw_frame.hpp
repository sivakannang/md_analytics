#pragma once

// A broker WebSocket frame exactly as received, for the recorder. Variable length:
// a fixed 32-byte RawFrameHeader followed by the frame bytes.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>

#include <mda/core/timestamp.hpp>
#include <mda/messages/message.hpp>

namespace mda::messages {

struct RawFrameHeader {
    EventHeader header;          ///< instrument id invalid; sequence = feed sequence
    core::Timestamp receive_ts;  ///< when the frame was read from the socket
    std::uint32_t connection_id = 0;
    std::uint32_t length = 0;  ///< frame bytes that follow
};

template <>
struct MessageTraits<RawFrameHeader> {
    static constexpr MessageType type = MessageType::RawFrame;
    static constexpr std::uint16_t version = 1;
};

static_assert(Message<RawFrameHeader>);
static_assert(sizeof(RawFrameHeader) == 32);
static_assert(offsetof(RawFrameHeader, receive_ts) == 16);
static_assert(offsetof(RawFrameHeader, connection_id) == 24);
static_assert(offsetof(RawFrameHeader, length) == 28);

/// Bytes needed to encode a frame of `frame_size` bytes.
[[nodiscard]] constexpr std::size_t raw_frame_size(std::size_t frame_size) noexcept {
    return sizeof(RawFrameHeader) + frame_size;
}

/// Writes header + frame into `out` (at least raw_frame_size(frame.size()) bytes) and
/// returns the encoded bytes. Sets `header.length` and the schema version.
[[nodiscard]] inline std::span<const std::byte> encode_raw_frame(
    std::span<std::byte> out, RawFrameHeader header, std::span<const std::byte> frame) noexcept {
    header.header.schema_version = MessageTraits<RawFrameHeader>::version;
    header.length = static_cast<std::uint32_t>(frame.size());
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), frame.data(), frame.size());
    return out.first(raw_frame_size(frame.size()));
}

struct RawFrameView {
    RawFrameHeader header;
    std::span<const std::byte> frame;  ///< points into the decoded payload
};

[[nodiscard]] inline std::expected<RawFrameView, DecodeError> decode_raw_frame(
    std::uint32_t type, std::span<const std::byte> payload) noexcept {
    if (type != to_wire(MessageType::RawFrame)) {
        return std::unexpected(DecodeError::WrongType);
    }
    if (payload.size() < sizeof(RawFrameHeader)) {
        return std::unexpected(DecodeError::WrongSize);
    }
    RawFrameHeader header;
    std::memcpy(&header, payload.data(), sizeof(header));
    if (header.header.schema_version != MessageTraits<RawFrameHeader>::version) {
        return std::unexpected(DecodeError::UnsupportedVersion);
    }
    if (payload.size() != raw_frame_size(header.length)) {
        return std::unexpected(DecodeError::WrongSize);
    }
    return RawFrameView{header, payload.subspan(sizeof(RawFrameHeader))};
}

}  // namespace mda::messages
