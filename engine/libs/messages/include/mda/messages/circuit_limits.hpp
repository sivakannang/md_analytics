#pragma once

#include <cstddef>

#include <mda/core/price.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/messages/message.hpp>

namespace mda::messages {

/// Today's price band of one instrument (lower / upper circuit). Not in the tick stream:
/// mda-feedd fetches it from the broker's quote API at start, for new subscriptions, and
/// periodically (bands can be revised intraday), and re-publishes it so late readers learn
/// it. 0 means "no band" (indices). 40 bytes.
struct CircuitLimits {
    EventHeader header;
    core::Timestamp fetched_ts;  ///< when mda-feedd fetched it
    core::Price lower;
    core::Price upper;
};

template <>
struct MessageTraits<CircuitLimits> {
    static constexpr MessageType type = MessageType::CircuitLimits;
    static constexpr std::uint16_t version = 1;
};

static_assert(Message<CircuitLimits>);
static_assert(sizeof(CircuitLimits) == 40);
static_assert(offsetof(CircuitLimits, fetched_ts) == 16);
static_assert(offsetof(CircuitLimits, lower) == 24);
static_assert(offsetof(CircuitLimits, upper) == 32);

}  // namespace mda::messages
