#pragma once

// MarketSignal, message type 9 on the `sig` bus (ADR-0028, docs/design/signals.md §5): one
// strategy's signal for one index and minute, with its four largest factor contributions.
// signals.db keeps every factor.

#include <cstddef>
#include <cstdint>

#include <mda/messages/message.hpp>
#include <mda/signals/strategy.hpp>

namespace mda::signals {

struct WireFactor {
    std::uint16_t factor = 0;  ///< FactorId; 0 = unused entry
    std::int16_t points = 0;   ///< weight × value, rounded
};

/// header.instrument_id = the spot; header.flags = the label (below). 48 bytes.
struct MarketSignal {
    messages::EventHeader header;
    std::int64_t minute = 0;  ///< minute start, Unix seconds (UTC)
    std::uint16_t strategy = 0;
    std::uint16_t horizon_min = 0;
    std::int16_t score = 0;    ///< -100 … +100
    std::uint16_t prob_bp = 0;  ///< calibrated P(up) in basis points; 0 = not calibrated
    WireFactor top[4] = {};     ///< largest |points| first
};

static_assert(sizeof(MarketSignal) == 48);
static_assert(offsetof(MarketSignal, minute) == 16);
static_assert(offsetof(MarketSignal, score) == 28);
static_assert(offsetof(MarketSignal, top) == 32);

/// header.flags: the label, as the strategy's thresholds set it (neither bit: Neutral).
inline constexpr std::uint16_t kSignalBullish = 1;
inline constexpr std::uint16_t kSignalBearish = 2;

[[nodiscard]] MarketSignal to_message(const Signal& signal, std::uint64_t sequence);

[[nodiscard]] Label label_of(const MarketSignal& message) noexcept;

}  // namespace mda::signals

namespace mda::messages {

template <>
struct MessageTraits<signals::MarketSignal> {
    static constexpr MessageType type = MessageType::MarketSignal;
    static constexpr std::uint16_t version = 1;
};
static_assert(Message<signals::MarketSignal>);

}  // namespace mda::messages
