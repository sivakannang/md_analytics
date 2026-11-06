#pragma once

// The compiled-in strategies by config id (ADR-0028), so mda-signald and the backtest
// build them the same way.

#include <memory>
#include <string_view>

#include <mda/signals/strategy.hpp>

namespace mda::signals {

/// The strategy `id` with its horizon and thresholds; nullptr for an unknown id.
[[nodiscard]] std::unique_ptr<Strategy> make_strategy(std::string_view id, int horizon_min,
                                                      int bullish, int bearish);

}  // namespace mda::signals
