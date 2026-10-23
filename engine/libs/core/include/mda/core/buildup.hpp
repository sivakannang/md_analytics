#pragma once

// What price and open interest did together (docs/design/oi-buildup-chart.md §1). Shared
// by the option chain's day change (ADR-0022) and the OI buildup chart; values are on the
// wire (bus messages, API), so only append.

#include <cstdint>

namespace mda::core {

enum class Buildup : std::uint8_t {
    None = 0,           ///< unknown (no OI, or nothing to compare with yet)
    LongBuildup = 1,    ///< price up, OI up
    ShortBuildup = 2,   ///< price down, OI up
    ShortCovering = 3,  ///< price up, OI down
    LongUnwinding = 4,  ///< price down, OI down
    Neutral = 5,        ///< OI change below the threshold
    NoOiUpdate = 6,     ///< no OI published in the bar
};

}  // namespace mda::core
