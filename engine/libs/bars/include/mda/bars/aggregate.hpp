#pragma once

// Higher timeframes from 1-minute bars (ADR-0021: aggregated at query time).
//
// Intraday buckets are aligned to the session start, 09:15 IST, like Kite's candles:
// 5m -> 09:15, 09:20, ...; 1h -> 09:15, 10:15, ..., 15:15. Daily buckets are IST days,
// stamped 00:00 IST.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <mda/bars/bar.hpp>

namespace mda::bars {

struct Interval {
    int minutes = 1;     ///< bucket length for intraday intervals
    bool daily = false;  ///< one bucket per IST day

    friend bool operator==(const Interval&, const Interval&) = default;
};

/// "1m", "3m", "5m", "15m", "30m", "1h", "1d"; nothing for anything else.
[[nodiscard]] std::optional<Interval> parse_interval(std::string_view text);

/// Start of the bucket containing `unix_seconds`.
[[nodiscard]] std::int64_t bucket_start(std::int64_t unix_seconds, Interval interval);

/// Merges 1-minute bars (one instrument, oldest first) into `interval` bars:
/// open = first, close = last, high/low = extremes, volume / OI updates / trades summed,
/// OI open/close = first/last known OI, OI high/low over bars that have OI, OI update
/// time = the last known one.
[[nodiscard]] std::vector<Bar> aggregate(std::span<const Bar> minutes, Interval interval);

}  // namespace mda::bars
