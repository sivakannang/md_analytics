#pragma once

// Synthetic bars and summaries for the signals tests.

#include <cmath>
#include <cstdint>

#include <mda/bars/bar.hpp>
#include <mda/options/chain_analytics.hpp>
#include <mda/signals/market_state.hpp>

namespace mda::signals::test {

inline constexpr std::int64_t kMidnightIst = 1'792'607'400;   // 2026-10-22 00:00 IST
inline constexpr std::int32_t kToday = 20'748;                // ist_day(kMidnightIst)
inline constexpr std::int64_t kOpen = kMidnightIst + 33'300;  // 09:15 IST

inline constexpr core::InstrumentId kSpot{1};
inline constexpr core::InstrumentId kFuture{2};

[[nodiscard]] inline std::int64_t at_min(std::int64_t minutes_after_open) {
    return kOpen + minutes_after_open * 60;
}

[[nodiscard]] inline core::Price price(double value) {
    return core::Price::from_raw(std::llround(value * static_cast<double>(core::Price::kScale)));
}

struct BarSpec {
    core::InstrumentId id;
    std::int64_t minute = 0;
    double close = 0;
    std::int64_t oi = 0;
    std::int64_t volume = 0;
    bool forming = false;
};

[[nodiscard]] inline bars::Bar bar(const BarSpec& spec) {
    bars::Bar result;
    result.header.instrument_id = spec.id;
    result.header.flags = spec.forming ? bars::kBarForming : std::uint16_t{0};
    result.start = spec.minute;
    result.open = price(spec.close);
    result.high = price(spec.close);
    result.low = price(spec.close);
    result.close = price(spec.close);
    result.volume = core::Quantity{spec.volume};
    result.oi_close = core::Quantity{spec.oi};
    return result;
}

struct SummarySpec {
    std::int32_t expiry_days = kToday + 7;
    double pcr = 0;
    double atm_strike = 0;
    double atm_iv = 0;
    double max_pain = 0;
};

[[nodiscard]] inline options::ChainSummaryMessage summary(const SummarySpec& spec) {
    options::ChainSummaryMessage result;
    result.header.instrument_id = kSpot;
    result.expiry_days = spec.expiry_days;
    result.pcr = spec.pcr;
    result.atm_strike = spec.atm_strike;
    result.atm_iv = spec.atm_iv;
    result.max_pain = spec.max_pain;
    return result;
}

[[nodiscard]] inline MarketState make_state() {
    return MarketState(MarketDefinition{.underlying = "NIFTY", .spot = kSpot, .future = kFuture});
}

}  // namespace mda::signals::test
