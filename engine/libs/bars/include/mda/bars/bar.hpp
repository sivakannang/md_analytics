#pragma once

// A 1-minute bar (ADR-0021; fields from docs/design/oi-buildup-chart.md §5), and its
// fixed-layout bus message for the analytics bus (ADR-0010 conventions).

#include <cstddef>
#include <cstdint>

#include <mda/core/price.hpp>
#include <mda/core/quantity.hpp>
#include <mda/messages/message.hpp>

namespace mda::bars {

/// Bar::oi_update_offset when there is no known OI update time.
inline constexpr std::uint16_t kNoOiUpdateTime = 0xFFFF;

enum class BarSource : std::uint8_t {
    Live = 1,  ///< built from our ticks
    Kite = 2,  ///< backfilled from the broker's historical API
};

/// One instrument's minute. Prices from LTP; volume is the traded quantity within the
/// minute; OI fields are 0 for instruments without OI.
struct Bar {
    messages::EventHeader header;  ///< instrument_id; flags: kBarForming while open
    std::int64_t start = 0;        ///< minute start, Unix seconds (UTC)
    core::Price open;
    core::Price high;
    core::Price low;
    core::Price close;
    core::Quantity volume;
    core::Quantity oi_open;
    core::Quantity oi_high;
    core::Quantity oi_low;
    core::Quantity oi_close;
    std::uint32_t oi_updates = 0;  ///< times OI changed within the minute
    std::uint32_t trades = 0;      ///< ticks that changed LTP or volume
    BarSource source = BarSource::Live;
    std::uint8_t reserved0 = 0;
    /// Seconds after `start` at which OI last changed, by receive time (Kite gives OI no
    /// timestamp); kNoOiUpdateTime when the bar has no OI update or it isn't known (backfill).
    std::uint16_t oi_update_offset = kNoOiUpdateTime;
    std::uint8_t reserved[4] = {};
};

/// header.flags: the bar is still forming (the current minute).
inline constexpr std::uint16_t kBarForming = 1;

static_assert(sizeof(Bar) == 112);
static_assert(offsetof(Bar, start) == 16);
static_assert(offsetof(Bar, oi_updates) == 96);
static_assert(offsetof(Bar, oi_update_offset) == 106);

}  // namespace mda::bars

namespace mda::messages {

template <>
struct MessageTraits<bars::Bar> {
    static constexpr MessageType type = MessageType::Bar;
    static constexpr std::uint16_t version = 2;  ///< 2: oi_update_offset
};

static_assert(Message<bars::Bar>);

}  // namespace mda::messages
