#pragma once

// How an auto-trader run did (docs/design/auto-trader.md §8): its fills grouped into round
// trips (flat → position → flat), P&L after charges, win rate, average win and loss, the
// largest loss, the maximum drawdown of cumulative net P&L, and exits by reason.

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <mda/autotrade/run.hpp>
#include <mda/broker/paper_store.hpp>

namespace mda::autotrade {

struct RoundTrip {
    Millis opened = 0;
    Millis closed = 0;
    std::int64_t quantity = 0;  ///< signed, as opened: + long, − short
    Paise gross = 0;            ///< realised by the closing fills
    Paise charges = 0;          ///< of every fill in the trip

    [[nodiscard]] Paise net() const noexcept { return gross - charges; }
};

struct Metrics {
    int round_trips = 0;
    int wins = 0;    ///< net > 0
    int losses = 0;  ///< net ≤ 0 (charges make a flat trade a loss)
    Paise gross = 0;    ///< all fills, open trip included
    Paise charges = 0;
    Paise net = 0;
    Paise average_win = 0;
    Paise average_loss = 0;  ///< negative
    Paise largest_loss = 0;  ///< negative, or 0
    Paise max_drawdown = 0;  ///< largest fall of cumulative net P&L over closed trips (positive)
    std::int64_t open_position = 0;
    std::map<std::string, int> exits;  ///< signal, target, max hold, end of day, stop, …
};

/// The closed round trips in `trades` (one instrument, oldest first).
[[nodiscard]] std::vector<RoundTrip> round_trips(std::span<const broker::TradeRow> trades);

/// The exit category of a decision ("target", "stop", …), or empty if it isn't an exit.
[[nodiscard]] std::string exit_category(const Decision& decision);

[[nodiscard]] Metrics metrics(std::span<const broker::TradeRow> trades, std::span<const Decision> decisions);

}  // namespace mda::autotrade
