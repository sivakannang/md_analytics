#pragma once

// Builds 1-minute bars from market snapshots (ADR-0021, docs/design/option-chain-and-bars.md §3).
//
// - The minute comes from the exchange timestamp (receive time only if there is none), so a
//   slow network can't move a trade into the wrong minute.
// - Only minutes inside the trading session produce bars: 09:15–15:30 IST for cash,
//   09:15–15:40 for derivatives (NSE and BSE F&O close at 15:40 since the closing auction
//   session started on 2026-08-03); MCX 09:00 to 23:30 / 23:55 (ADR-0031), through `session`.
// - Volume per minute is the rise in Kite's cumulative day volume. The baseline only goes
//   up: during the closing auction Kite's volume field drops (to the auction's matched
//   quantity) and returns to the day total after it, which must not count as volume.
// - A minute closes when a tick from a later minute arrives, or `close_delay` after it
//   ends (close_due); minutes without ticks produce no bar.
// - A minute where only OI changed (no trade) still makes a bar: OHLC = previous close.
// - After a feed outage (no tick from any instrument for `feed_gap`, e.g. the Wi-Fi was
//   down), the first tick of each instrument is a new volume baseline: the volume traded
//   meanwhile isn't one minute's, and a backfill fills the minutes missed.
//
// Single-threaded: call everything from one thread.

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <mda/bars/bar.hpp>
#include <mda/core/instrument_id.hpp>
#include <mda/messages/market_snapshot.hpp>

namespace mda::bars {

struct BuilderOptions {
    /// Trading session in minutes since IST midnight: [start, end).
    int session_start = 9 * 60 + 15;
    int session_end = 15 * 60 + 30;
    int derivatives_session_end = 15 * 60 + 40;
    /// Whether an instrument is a future or option (session ends at derivatives_session_end);
    /// unset or false = cash. Asked once per instrument.
    std::function<bool(core::InstrumentId)> is_derivative;
    /// When set, an instrument's session [start, end) in minutes of the IST day holding
    /// `minute` (Unix seconds), per exchange; overrides the three fields above. Asked once per
    /// instrument.
    std::function<std::pair<int, int>(core::InstrumentId, std::int64_t minute)> session;
    std::chrono::seconds close_delay{2};
    /// No tick from any instrument for this long (receive time) is a feed outage; 0 = off.
    std::chrono::seconds feed_gap{0};
};

class BarBuilder {
public:
    using Sink = std::function<void(const Bar&)>;

    BarBuilder(BuilderOptions options, Sink on_closed);

    void on_snapshot(const messages::MarketSnapshot& snapshot);

    /// Closes every bar whose minute ended at least close_delay before `now` (Unix seconds).
    void close_due(std::int64_t now);

    /// Bars still forming (for the 1-second forming-bar updates).
    [[nodiscard]] std::vector<Bar> forming() const;

    /// Seeds an instrument's state from its last closed bar (after a restart), so OI-only
    /// minutes and OI-update counting continue correctly. `day_volume` is the volume of
    /// today's stored bars: the first tick's cumulative volume becomes the baseline unless it
    /// is lower (a restart during the closing auction), so neither the minutes missed while
    /// down nor the auction's drop land in a bar as volume.
    void seed(const Bar& last_closed, std::int64_t day_volume = 0);

    [[nodiscard]] std::size_t instruments() const noexcept { return states_.size(); }

private:
    struct State {
        Bar bar{};  // the forming bar (valid when open)
        bool open = false;
        std::int64_t last_volume = -1;    // highest cumulative day volume seen; -1 unknown
        std::int64_t seeded_volume = -1;  // from seed(), until the first volume tick
        int session_start = -1;           // minute of day; -1 until first seen
        int session_end = -1;
        core::Price last_close;     // last traded price seen (carried into OI-only bars)
        std::int64_t last_oi = -1;  // -1 unknown
        bool priced = false;        // the forming bar has a price
    };

    void close(State& state);

    BuilderOptions options_;
    Sink on_closed_;
    std::int64_t last_received_ = 0;  // any instrument's latest receive time, Unix seconds
    std::unordered_map<std::uint32_t, State> states_;
};

/// IST minute-of-day of a Unix time.
[[nodiscard]] int ist_minute_of_day(std::int64_t unix_seconds);

}  // namespace mda::bars
