#pragma once

// One underlying's rolling market state for the signal strategies (docs/design/signals.md
// §2–§3): closed 1-minute bars of the spot, its near-month future, its options, the
// sector indices and India VIX (§13), plus the future's signed volume from its
// snapshots and depth (F9, §13), plus the engine's chain summaries. Every query takes the
// evaluation minute `t` and sees only data stamped at or before `t`, on the same IST day, so a
// strategy can't look ahead.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <mda/bars/bar.hpp>
#include <mda/core/instrument_id.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/options/chain_analytics.hpp>

namespace mda::signals {

inline constexpr std::int64_t kIstOffset = 19'800;
inline constexpr std::int64_t kSecondsPerDay = 86'400;
inline constexpr std::int64_t kSessionOpen = 9 * 3600 + 15 * 60;  ///< 09:15 IST, after the pre-open
/// No snapshot from any instrument for this long is a feed outage (seconds): F9 rebaselines.
inline constexpr std::int64_t kFeedGap = 120;

/// IST calendar day of Unix time `t`, as days since 1970-01-01 (same scale as expiry_days).
[[nodiscard]] constexpr std::int32_t ist_day(std::int64_t t) noexcept {
    const std::int64_t local = t + kIstOffset;
    const std::int64_t day = local / kSecondsPerDay - (local % kSecondsPerDay < 0 ? 1 : 0);
    return static_cast<std::int32_t>(day);
}

/// Unix time of 00:00 IST on the day containing `t`.
[[nodiscard]] constexpr std::int64_t ist_day_start(std::int64_t t) noexcept {
    return std::int64_t{ist_day(t)} * kSecondsPerDay - kIstOffset;
}

/// Values stamped by minute start (Unix seconds), kept in time order.
template <typename T>
class Series {
public:
    struct Point {
        std::int64_t minute = 0;
        T value{};
    };

    /// Adds or replaces the value at `minute` (backfill and live may both send a minute).
    void add(std::int64_t minute, T value) {
        auto it = std::lower_bound(points_.begin(), points_.end(), minute,
                                   [](const Point& p, std::int64_t m) { return p.minute < m; });
        if (it != points_.end() && it->minute == minute) {
            it->value = std::move(value);
        } else {
            points_.insert(it, Point{.minute = minute, .value = std::move(value)});
        }
    }

    /// The latest value stamped in [from, t]; nullptr when there is none.
    [[nodiscard]] const T* at(std::int64_t t, std::int64_t from) const {
        const auto range = between(from, t);
        return range.empty() ? nullptr : &range.back().value;
    }

    /// Points stamped in [from, t], oldest first.
    [[nodiscard]] std::span<const Point> between(std::int64_t from, std::int64_t t) const {
        const auto by_minute = [](const Point& p, std::int64_t m) { return p.minute < m; };
        const auto first = std::lower_bound(points_.begin(), points_.end(), from, by_minute);
        const auto last =
            std::upper_bound(points_.begin(), points_.end(), t,
                             [](std::int64_t m, const Point& p) { return m < p.minute; });
        if (first >= last) {
            return {};
        }
        return {first, last};
    }

    /// The value at `minute`, added (default) when there is none.
    T& upsert(std::int64_t minute) {
        auto it = std::lower_bound(points_.begin(), points_.end(), minute,
                                   [](const Point& p, std::int64_t m) { return p.minute < m; });
        if (it == points_.end() || it->minute != minute) {
            it = points_.insert(it, Point{.minute = minute, .value = T{}});
        }
        return it->value;
    }

    /// Drops points stamped before `before`.
    void prune(std::int64_t before) {
        const auto it =
            std::lower_bound(points_.begin(), points_.end(), before,
                             [](const Point& p, std::int64_t m) { return p.minute < m; });
        points_.erase(points_.begin(), it);
    }

    [[nodiscard]] bool empty() const noexcept { return points_.empty(); }

private:
    std::vector<Point> points_;
};

struct OptionInfo {
    core::InstrumentId id;
    std::int32_t expiry_days = 0;  ///< days since 1970-01-01
    double strike = 0;
    bool call = false;
};

struct FutureBar {
    double close = 0;
    double typical = 0;  ///< (high + low + close) / 3, for VWAP
    std::int64_t volume = 0;
    std::int64_t oi = 0;
};

struct SummaryPoint {
    double forward = 0;
    double atm_strike = 0;
    double atm_iv = 0;
    double pcr = 0;
    double max_pain = 0;
};

/// The future's traded volume in one minute, by aggressor (F9).
struct FlowMinute {
    std::int64_t buy = 0;
    std::int64_t sell = 0;
    std::int64_t unclassified = 0;
};

enum class Aggressor : std::int8_t { Sell = -1, Unknown = 0, Buy = 1 };

/// Who initiated a snapshot's new volume (signals.md §13, Lee–Ready on snapshots): the quote
/// rule against the best bid and ask before it (0 when unknown), then the tick rule:
/// `tick` is the sign of the last LTP change (0 when there was none).
[[nodiscard]] Aggressor classify_trade(std::int64_t ltp, std::int64_t bid, std::int64_t ask,
                                       std::int64_t prev_ltp, int tick) noexcept;

struct MarketDefinition {
    std::string underlying;  ///< NIFTY, SENSEX …
    core::InstrumentId spot;
    core::InstrumentId future;  ///< near month; invalid when none is subscribed
};

class MarketState {
public:
    struct OptionEntry {
        OptionInfo info;
        Series<std::int64_t> oi;
    };
    struct SectorEntry {
        core::InstrumentId id;
        double prev_close = 0;
        Series<double> close;
    };

    explicit MarketState(MarketDefinition definition);

    /// The near-month future changes at expiry; its history starts again.
    void set_future(core::InstrumentId future);
    /// Chains roll with ATM, so options arrive while running. Re-adding updates the info.
    void add_option(const OptionInfo& option);
    /// A sector index for breadth, with its previous trading day's close.
    void add_sector(core::InstrumentId id, double prev_close);
    /// India VIX, shared by every underlying (F8).
    void set_vix(core::InstrumentId id);

    /// A bar of any instrument; forming bars and unknown instruments are ignored.
    void on_bar(const bars::Bar& bar);
    /// The near-month future's snapshots and depth (from `md`); other instruments only mark
    /// the feed alive (an outage of kFeedGap makes the next volume a baseline). A snapshot's
    /// new volume is classified against the depth before it.
    void on_snapshot(const messages::MarketSnapshot& snapshot);
    void on_depth(const messages::DepthSnapshot& depth);
    /// Messages were lost (the reader was lapped): the next snapshot only sets the baseline.
    void reset_flow_baseline() noexcept;

    /// The engine's summary of one expiry; ignored unless it is this market's spot.
    /// `minute` is the bar minute it belongs to (the message carries no time).
    void on_summary(const options::ChainSummaryMessage& summary, std::int64_t minute);

    /// Drops everything stamped before `before` (call at the start of a new day).
    void prune(std::int64_t before);

    [[nodiscard]] const MarketDefinition& definition() const noexcept { return definition_; }
    [[nodiscard]] const Series<double>& spot() const noexcept { return spot_; }
    [[nodiscard]] const Series<FutureBar>& future() const noexcept { return future_; }
    [[nodiscard]] const std::vector<OptionEntry>& options() const noexcept { return options_; }
    [[nodiscard]] const std::vector<SectorEntry>& sectors() const noexcept { return sectors_; }
    [[nodiscard]] const Series<double>& vix() const noexcept { return vix_; }
    [[nodiscard]] core::InstrumentId vix_id() const noexcept { return vix_id_; }
    /// The future's signed volume by minute (exchange time).
    [[nodiscard]] const Series<FlowMinute>& flow() const noexcept { return flow_; }

    /// The nearest expiry not yet past at `t` that has a summary on t's day by `t`.
    [[nodiscard]] std::optional<std::int32_t> nearest_expiry(std::int64_t t) const;
    /// That expiry's summary series; nullptr when there is none.
    [[nodiscard]] const Series<SummaryPoint>* summaries(std::int32_t expiry_days) const;
    /// Distinct strikes of one expiry, ascending.
    [[nodiscard]] std::vector<double> strikes(std::int32_t expiry_days) const;

private:
    enum class Role : std::uint8_t { Spot, Future, Option, Sector, Vix };
    struct Slot {
        Role role = Role::Spot;
        std::size_t index = 0;
    };

    MarketDefinition definition_;
    Series<double> spot_;
    Series<FutureBar> future_;
    std::vector<OptionEntry> options_;
    std::vector<SectorEntry> sectors_;
    Series<double> vix_;
    core::InstrumentId vix_id_;

    // F9 classifier state for the future: the previous snapshot and the depth since.
    struct FlowTracker {
        std::int64_t volume = -1;  ///< cumulative day volume; -1 = no baseline
        std::int64_t ltp = 0;
        int tick = 0;          ///< sign of the last LTP change
        std::int64_t bid = 0;  ///< best bid and ask, raw; 0 = none
        std::int64_t ask = 0;
    };
    Series<FlowMinute> flow_;
    FlowTracker tracker_;
    std::int64_t last_seen_ = 0;  ///< any snapshot's latest receive time, Unix seconds
    std::unordered_map<std::int32_t, Series<SummaryPoint>> summaries_;
    std::unordered_map<std::uint32_t, Slot> slots_;
};

}  // namespace mda::signals
