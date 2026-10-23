#pragma once

// Common machinery for bus messages (ADR-0010, docs/design/domain-model.md §3).
//
// Every message is a fixed-layout, trivially copyable struct starting with an
// EventHeader. Encoding is "view the struct as bytes"; decoding is "check type, size and
// schema version, then copy the bytes into a struct". No serialization library.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <type_traits>

#include <mda/core/instrument_id.hpp>

namespace mda::messages {

/// Bus message type ids (the bus's MsgType). 0 is reserved by the bus for padding.
enum class MessageType : std::uint32_t {
    RawFrame = 1,
    MarketSnapshot = 2,
    DepthSnapshot = 3,
    FeedStatus = 4,
    CircuitLimits = 5,
    Bar = 6,              ///< bars::Bar on the analytics bus (mda-engine)
    OptionAnalytics = 7,  ///< options::OptionAnalyticsMessage (mda-engine)
    ChainSummary = 8,     ///< options::ChainSummaryMessage (mda-engine)
};

[[nodiscard]] constexpr std::uint32_t to_wire(MessageType type) noexcept {
    return static_cast<std::uint32_t>(type);
}

/// First 16 bytes of every message.
struct EventHeader {
    std::uint16_t schema_version = 0;  ///< per message type; readers reject unknown versions
    std::uint16_t flags = 0;           ///< message-specific
    core::InstrumentId instrument_id;  ///< invalid (0) for non-instrument events
    std::uint64_t sequence = 0;        ///< per-feed sequence from mda-feedd
};
static_assert(sizeof(EventHeader) == 16);
static_assert(offsetof(EventHeader, instrument_id) == 4);
static_assert(offsetof(EventHeader, sequence) == 8);

/// Each message struct specializes this with its type id and current schema version.
template <typename T>
struct MessageTraits;

template <typename T>
concept Message = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> && requires {
    { MessageTraits<T>::type } -> std::convertible_to<MessageType>;
    { MessageTraits<T>::version } -> std::convertible_to<std::uint16_t>;
} && std::is_same_v<decltype(T::header), EventHeader>;

/// Header for a new message of type T, stamped with T's current schema version.
template <Message T>
[[nodiscard]] constexpr EventHeader make_header(core::InstrumentId instrument,
                                                std::uint64_t sequence) noexcept {
    return {.schema_version = MessageTraits<T>::version,
            .flags = 0,
            .instrument_id = instrument,
            .sequence = sequence};
}

/// The message's bytes, ready to publish. Valid as long as `message` is.
template <Message T>
[[nodiscard]] std::span<const std::byte> as_bytes(const T& message) noexcept {
    return std::as_bytes(std::span(&message, 1));
}

enum class DecodeError {
    WrongType,           ///< bus message type is not T's
    WrongSize,           ///< payload size differs from sizeof(T)
    UnsupportedVersion,  ///< schema_version not the one this build understands
};

/// Copies a payload into a T after checking type, size and schema version.
/// The copy (not a reinterpret_cast) makes alignment and object-lifetime rules moot.
template <Message T>
[[nodiscard]] std::expected<T, DecodeError> decode(std::uint32_t type,
                                                   std::span<const std::byte> payload) noexcept {
    if (type != to_wire(MessageTraits<T>::type)) {
        return std::unexpected(DecodeError::WrongType);
    }
    if (payload.size() != sizeof(T)) {
        return std::unexpected(DecodeError::WrongSize);
    }
    T message;
    std::memcpy(&message, payload.data(), sizeof(T));
    if (message.header.schema_version != MessageTraits<T>::version) {
        return std::unexpected(DecodeError::UnsupportedVersion);
    }
    return message;
}

}  // namespace mda::messages
