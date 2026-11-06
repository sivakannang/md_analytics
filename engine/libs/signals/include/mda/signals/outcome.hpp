#pragma once

// What happened after a signal (docs/design/signals.md §6): the spot's return from the
// signal's minute to minute + horizon, or to the day's close if that comes first, and
// whether a Bullish or Bearish call was right. Prices are core::Price raw values.

#include <cstdint>
#include <optional>
#include <span>

#include <mda/signals/strategy.hpp>

namespace mda::signals {

/// Start of the cash session's last minute, seconds after 00:00 IST (bars of the index
/// spots end at 15:30). Once that minute has closed, a day's open signals settle.
inline constexpr std::int64_t kCashLastMinute = 15 * 3600 + 29 * 60;

struct Outcome {
    std::int64_t spot_after_raw = 0;
    std::optional<int> return_bp;  ///< empty when the signal's spot is unknown
    std::optional<bool> hit;       ///< empty for Neutral, a flat move, or no return
};

/// The outcome of a signal given at `spot_raw` and settled at `spot_after_raw`. A move
/// within ±`flat_bp` is flat: neither a hit nor a miss.
[[nodiscard]] Outcome outcome(Label label, std::int64_t spot_raw, std::int64_t spot_after_raw,
                              int flat_bp);

/// One closed spot bar: its minute and close.
struct SpotClose {
    std::int64_t minute = 0;
    std::int64_t close_raw = 0;
};

/// Whether the day of `signal_minute` has ended, seen from the newest bar minute: a later
/// day, or the cash session's last minute closed.
[[nodiscard]] bool day_closed(std::int64_t signal_minute, std::int64_t latest_minute);

/// The price a signal settles at, from its day's spot closes (oldest first): the first
/// close at or after minute + horizon (a missing bar moves it to the next one); if the day
/// closed before that, the day's last close. Empty: not yet.
[[nodiscard]] std::optional<std::int64_t> settle_price(std::span<const SpotClose> day,
                                                       std::int64_t minute, int horizon_min,
                                                       bool closed);

}  // namespace mda::signals
