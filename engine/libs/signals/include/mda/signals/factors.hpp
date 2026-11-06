#pragma once

// Direction factors F1–F9 (docs/design/signals.md §4, §13). Each reads a MarketState at minute
// `t` and gives a value in [-1, 1], bullish positive, squashed with tanh so one extreme
// reading can't dominate. Strategies weight them; ids are on the wire, so only append.

#include <cstdint>
#include <string_view>

#include <mda/signals/market_state.hpp>

namespace mda::signals {

enum class FactorId : std::uint16_t {
    FuturesBuildup = 1,  ///< F1: near-month future's price and OI over the buildup window
    AtmOiShift = 2,      ///< F2: put minus call OI change around ATM, nearest expiry
    PcrChange = 3,       ///< F3: PCR change, nearest expiry
    FutureVsVwap = 4,    ///< F4: future's close against its day VWAP
    AtmIvChange = 5,     ///< F5: ATM IV change (falling is bullish)
    MaxPainPull = 6,     ///< F6: max pain against spot, close to expiry only
    SectorBreadth = 7,   ///< F7: share of sector indices up on the day
    VixChange = 8,       ///< F8: India VIX change (rising is bearish)
    VolumeDelta = 9,     ///< F9: the future's buyer- minus seller-initiated volume
};
inline constexpr std::size_t kFactorCount = 9;

enum class FactorStatus : std::uint8_t {
    Present = 0,
    Absent = 1,         ///< the data isn't there (yet): no future, too early in the day …
    NotApplicable = 2,  ///< the factor doesn't apply now (max pain far from expiry)
};

struct FactorValue {
    FactorId id = FactorId::FuturesBuildup;
    FactorStatus status = FactorStatus::Absent;
    double value = 0;  ///< [-1, 1]; 0 unless Present
};

/// Windows and scales; the scales are starting guesses (§4), set from real data later.
struct FactorParams {
    std::int64_t buildup_window_min = 15;
    double buildup_oi_fraction = 0.001;  ///< smaller OI changes are neutral
    std::int64_t oi_window_min = 30;     ///< F2, F3 and F5
    int atm_strikes = 5;                 ///< F2: strikes each side of ATM
    double oi_shift_scale = 0.02;
    double pcr_scale = 0.10;
    double vwap_scale = 0.002;
    double iv_scale = 0.01;
    double max_pain_scale = 0.005;
    int max_pain_days = 1;  ///< F6 applies this many calendar days before expiry and on it
    int min_sectors = 3;    ///< F7 needs at least this many sectors with data
    std::int64_t vix_window_min = 30;
    std::int64_t flow_window_min = 15;  ///< F9: closed minutes ending at t
    int flow_min_minutes = 10;          ///< F9 needs volume in at least this many of them
    double flow_scale = 0.3;            ///< F9: twice the median |imbalance| (signals.md §13)
    double vix_scale = 0.013;  ///< F8: twice the median |30-minute VIX change| (signals.md §13)
};

[[nodiscard]] FactorValue futures_buildup(const MarketState& state, std::int64_t t,
                                          const FactorParams& params);
[[nodiscard]] FactorValue atm_oi_shift(const MarketState& state, std::int64_t t,
                                       const FactorParams& params);
[[nodiscard]] FactorValue pcr_change(const MarketState& state, std::int64_t t,
                                     const FactorParams& params);
[[nodiscard]] FactorValue future_vs_vwap(const MarketState& state, std::int64_t t,
                                         const FactorParams& params);
[[nodiscard]] FactorValue atm_iv_change(const MarketState& state, std::int64_t t,
                                        const FactorParams& params);
[[nodiscard]] FactorValue max_pain_pull(const MarketState& state, std::int64_t t,
                                        const FactorParams& params);
[[nodiscard]] FactorValue sector_breadth(const MarketState& state, std::int64_t t,
                                         const FactorParams& params);

[[nodiscard]] FactorValue vix_change(const MarketState& state, std::int64_t t,
                                     const FactorParams& params);

[[nodiscard]] FactorValue volume_delta(const MarketState& state, std::int64_t t,
                                       const FactorParams& params);

/// Every factor, in id order.
[[nodiscard]] FactorValue evaluate_factor(FactorId id, const MarketState& state, std::int64_t t,
                                          const FactorParams& params);

[[nodiscard]] std::string_view name(FactorId id) noexcept;

}  // namespace mda::signals
