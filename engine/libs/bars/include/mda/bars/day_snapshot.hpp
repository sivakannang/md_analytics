#pragma once

// A last-known quote built from one IST day's 1-minute bars (ADR-0024): the fallback for
// instruments with no saved quote (a new install, or a quiet instrument), so the watchlist
// and option chains have a value before the first tick.

#include <optional>
#include <span>

#include <mda/bars/bar.hpp>
#include <mda/core/price.hpp>
#include <mda/messages/market_snapshot.hpp>

namespace mda::bars {

/// LTP = the last close, day open/high/low/volume and OI from `day` (one instrument,
/// oldest first, not empty), `previous_close` for the day change (0 when unknown), and
/// exchange and receive time = the end of the last minute.
[[nodiscard]] messages::MarketSnapshot day_snapshot(std::span<const Bar> day,
                                                    core::Price previous_close);

}  // namespace mda::bars
