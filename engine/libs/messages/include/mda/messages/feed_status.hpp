#pragma once

#include <cstddef>
#include <cstdint>

#include <mda/core/timestamp.hpp>
#include <mda/messages/message.hpp>

namespace mda::messages {

enum class FeedState : std::uint8_t {
    Connecting = 1,
    Connected = 2,
    Disconnected = 3,
    Stale = 4,        ///< connected but no data for longer than expected
    AuthExpired = 5,  ///< session token rejected; waiting for the daily login
};

/// Health of one broker connection. Instrument id is invalid (0). 40 bytes.
struct FeedStatus {
    EventHeader header;
    core::Timestamp timestamp;
    std::uint32_t connection_id = 0;
    FeedState state = FeedState::Connecting;
    std::uint8_t reserved[3] = {};
    std::uint32_t subscribed_instruments = 0;
    std::uint32_t reconnects = 0;  ///< since process start
};

template <>
struct MessageTraits<FeedStatus> {
    static constexpr MessageType type = MessageType::FeedStatus;
    static constexpr std::uint16_t version = 1;
};

static_assert(Message<FeedStatus>);
static_assert(sizeof(FeedStatus) == 40);
static_assert(offsetof(FeedStatus, timestamp) == 16);
static_assert(offsetof(FeedStatus, connection_id) == 24);
static_assert(offsetof(FeedStatus, state) == 28);
static_assert(offsetof(FeedStatus, subscribed_instruments) == 32);
static_assert(offsetof(FeedStatus, reconnects) == 36);

}  // namespace mda::messages
