#include <cmath>

#include <gtest/gtest.h>

#include <mda/signals/factors.hpp>

#include "test_support.hpp"

namespace mda::signals {
namespace {

using namespace test;

const FactorParams kParams{};

// Future bars 15 minutes apart: price and OI before and now.
MarketState with_future(double close_then, std::int64_t oi_then, double close_now, std::int64_t oi_now) {
    auto state = make_state();
    state.on_bar(bar({.id = kFuture, .minute = at_min(0), .close = close_then, .oi = oi_then, .volume = 10}));
    state.on_bar(bar({.id = kFuture, .minute = at_min(15), .close = close_now, .oi = oi_now, .volume = 10}));
    return state;
}

TEST(FuturesBuildup, ScoresEachState) {
    const auto t = at_min(15);
    EXPECT_DOUBLE_EQ(futures_buildup(with_future(100, 1000, 101, 1100), t, kParams).value, 1.0);
    EXPECT_DOUBLE_EQ(futures_buildup(with_future(100, 1000, 99, 1100), t, kParams).value, -1.0);
    EXPECT_DOUBLE_EQ(futures_buildup(with_future(100, 1000, 101, 900), t, kParams).value, 0.5);
    EXPECT_DOUBLE_EQ(futures_buildup(with_future(100, 1000, 99, 900), t, kParams).value, -0.5);
}

TEST(FuturesBuildup, SmallOiChangeIsNeutral) {
    const auto value = futures_buildup(with_future(100, 100'000, 101, 100'050), at_min(15), kParams);
    EXPECT_EQ(value.status, FactorStatus::Present);
    EXPECT_DOUBLE_EQ(value.value, 0);
}

TEST(FuturesBuildup, AbsentWithoutHistoryToday) {
    auto state = make_state();
    // Yesterday's last bar must not stand in for 15 minutes ago.
    state.on_bar(bar({.id = kFuture, .minute = kOpen - 18 * 3600, .close = 90, .oi = 500}));
    state.on_bar(bar({.id = kFuture, .minute = at_min(5), .close = 100, .oi = 1000}));
    EXPECT_EQ(futures_buildup(state, at_min(5), kParams).status, FactorStatus::Absent);
    EXPECT_EQ(futures_buildup(make_state(), at_min(5), kParams).status, FactorStatus::Absent);
}

TEST(AtmOiShift, PutWritingAroundAtmIsBullish) {
    auto state = make_state();
    const std::int32_t expiry = kToday + 7;
    for (int strike = 800; strike <= 1200; strike += 100) {
        state.add_option({.id = core::InstrumentId{static_cast<std::uint32_t>(strike)},
                          .expiry_days = expiry, .strike = static_cast<double>(strike), .call = true});
    }
    const core::InstrumentId put_atm{5000};
    const core::InstrumentId put_far{5001};
    state.add_option({.id = put_atm, .expiry_days = expiry, .strike = 1000, .call = false});
    state.add_option({.id = put_far, .expiry_days = expiry, .strike = 1500, .call = false});

    const core::InstrumentId call_atm{1000};
    state.on_bar(bar({.id = put_atm, .minute = at_min(0), .close = 5, .oi = 1000}));
    state.on_bar(bar({.id = call_atm, .minute = at_min(0), .close = 5, .oi = 1000}));
    state.on_bar(bar({.id = put_far, .minute = at_min(0), .close = 5, .oi = 5000}));
    state.on_bar(bar({.id = put_atm, .minute = at_min(30), .close = 5, .oi = 1300}));
    state.on_bar(bar({.id = call_atm, .minute = at_min(30), .close = 5, .oi = 900}));
    state.on_bar(bar({.id = put_far, .minute = at_min(30), .close = 5, .oi = 9000}));  // outside ATM ± 2
    state.on_summary(summary({.expiry_days = expiry, .pcr = 1, .atm_strike = 1000}), at_min(30));

    FactorParams params;
    params.atm_strikes = 2;
    params.oi_shift_scale = 1;
    const auto value = atm_oi_shift(state, at_min(30), params);
    ASSERT_EQ(value.status, FactorStatus::Present);
    EXPECT_NEAR(value.value, std::tanh((300.0 + 100.0) / 2200.0), 1e-12);
}

TEST(AtmOiShift, AbsentWithoutOptions) {
    auto state = make_state();
    state.on_summary(summary({.pcr = 1, .atm_strike = 1000}), at_min(30));
    EXPECT_EQ(atm_oi_shift(state, at_min(30), kParams).status, FactorStatus::Absent);
}

TEST(PcrChange, RisingPcrIsBullish) {
    auto state = make_state();
    state.on_summary(summary({.pcr = 0.9}), at_min(0));
    state.on_summary(summary({.pcr = 1.0}), at_min(30));
    EXPECT_NEAR(pcr_change(state, at_min(30), kParams).value, std::tanh(1.0), 1e-12);
    EXPECT_EQ(pcr_change(state, at_min(29), kParams).status, FactorStatus::Absent);  // window not full
}

TEST(FutureVsVwap, AboveVwapIsBullish) {
    const auto state = with_future(100, 1000, 101, 1000);  // VWAP 100.5
    EXPECT_NEAR(future_vs_vwap(state, at_min(15), kParams).value,
                std::tanh((101 - 100.5) / 100.5 / kParams.vwap_scale), 1e-12);
}

TEST(FutureVsVwap, AbsentWithoutVolume) {
    auto state = make_state();
    state.on_bar(bar({.id = kFuture, .minute = at_min(0), .close = 100, .oi = 1000}));
    EXPECT_EQ(future_vs_vwap(state, at_min(0), kParams).status, FactorStatus::Absent);
}

TEST(AtmIvChange, FallingIvIsBullish) {
    auto state = make_state();
    state.on_summary(summary({.pcr = 1, .atm_iv = 0.15}), at_min(0));
    state.on_summary(summary({.pcr = 1, .atm_iv = 0.14}), at_min(30));
    EXPECT_NEAR(atm_iv_change(state, at_min(30), kParams).value, std::tanh(1.0), 1e-9);
}

TEST(MaxPainPull, OnlyNearExpiry) {
    auto state = make_state();
    state.on_bar(bar({.id = kSpot, .minute = at_min(0), .close = 1000}));
    state.on_summary(summary({.expiry_days = kToday + 7, .pcr = 1, .max_pain = 1010}), at_min(0));
    EXPECT_EQ(max_pain_pull(state, at_min(0), kParams).status, FactorStatus::NotApplicable);

    state.on_summary(summary({.expiry_days = kToday, .pcr = 1, .max_pain = 1010}), at_min(0));
    EXPECT_NEAR(max_pain_pull(state, at_min(0), kParams).value, std::tanh(0.01 / 0.005), 1e-12);
}

TEST(SectorBreadth, ShareOfSectorsUp) {
    auto state = make_state();
    for (std::uint32_t i = 0; i < 4; ++i) {
        const core::InstrumentId id{200 + i};
        state.add_sector(id, 100);
        state.on_bar(bar({.id = id, .minute = at_min(0), .close = i < 3 ? 101.0 : 99.0}));
    }
    EXPECT_DOUBLE_EQ(sector_breadth(state, at_min(0), kParams).value, 0.5);  // 3 of 4 up
}

TEST(SectorBreadth, AbsentWithTooFewSectors) {
    auto state = make_state();
    state.add_sector(core::InstrumentId{200}, 100);
    state.on_bar(bar({.id = core::InstrumentId{200}, .minute = at_min(0), .close = 101}));
    EXPECT_EQ(sector_breadth(state, at_min(0), kParams).status, FactorStatus::Absent);
}

constexpr core::InstrumentId kVix{300};

MarketState with_vix(double then, double now) {
    auto state = make_state();
    state.set_vix(kVix);
    state.on_bar(bar({.id = kVix, .minute = at_min(0), .close = then}));
    state.on_bar(bar({.id = kVix, .minute = at_min(30), .close = now}));
    return state;
}

TEST(VixChange, RisingVixIsBearish) {
    const auto rising = vix_change(with_vix(14, 14.14), at_min(30), kParams);  // +1 %
    EXPECT_EQ(rising.status, FactorStatus::Present);
    EXPECT_NEAR(rising.value, -std::tanh(0.01 / 0.013), 1e-9);
    EXPECT_NEAR(vix_change(with_vix(14, 13.86), at_min(30), kParams).value, std::tanh(0.01 / 0.013), 1e-9);
    EXPECT_DOUBLE_EQ(vix_change(with_vix(14, 14), at_min(30), kParams).value, 0);
}

TEST(VixChange, ScaleComesFromParams) {
    FactorParams params;
    params.vix_scale = 0.02;
    EXPECT_NEAR(vix_change(with_vix(14, 14.14), at_min(30), params).value, -std::tanh(0.5), 1e-9);
}

TEST(VixChange, AbsentUntilTheWindowIsFull) {
    // 29 minutes after the first bar there is no bar 30 minutes back on the same day.
    EXPECT_EQ(vix_change(with_vix(14, 14.14), at_min(29), kParams).status, FactorStatus::Absent);
    auto state = make_state();  // no VIX configured
    EXPECT_EQ(vix_change(state, at_min(30), kParams).status, FactorStatus::Absent);
}

TEST(VixChange, IgnoresVixBarsUntilConfigured) {
    auto state = make_state();
    state.on_bar(bar({.id = kVix, .minute = at_min(0), .close = 14}));
    state.set_vix(kVix);
    state.on_bar(bar({.id = kVix, .minute = at_min(30), .close = 15}));
    EXPECT_EQ(vix_change(state, at_min(30), kParams).status, FactorStatus::Absent);
    EXPECT_EQ(state.vix_id(), kVix);
}

TEST(Factors, NoLookAhead) {
    auto state = with_future(100, 1000, 101, 1100);
    state.on_summary(summary({.pcr = 0.9, .atm_iv = 0.15}), at_min(0));
    state.on_summary(summary({.pcr = 1.0, .atm_iv = 0.14}), at_min(15));
    state.set_vix(kVix);
    state.on_bar(bar({.id = kVix, .minute = at_min(-15), .close = 14}));
    state.on_bar(bar({.id = kVix, .minute = at_min(15), .close = 14.1}));
    const auto t = at_min(15);
    std::vector<double> before;
    for (std::size_t i = 1; i <= kFactorCount; ++i) {
        before.push_back(evaluate_factor(static_cast<FactorId>(i), state, t, kParams).value);
    }
    // Data from later minutes, pointing the other way, must not change the values at t.
    state.on_bar(bar({.id = kFuture, .minute = at_min(16), .close = 80, .oi = 5000, .volume = 1000}));
    state.on_summary(summary({.pcr = 0.5, .atm_iv = 0.30}), at_min(16));
    state.on_bar(bar({.id = kVix, .minute = at_min(16), .close = 20}));
    for (std::size_t i = 1; i <= kFactorCount; ++i) {
        EXPECT_DOUBLE_EQ(evaluate_factor(static_cast<FactorId>(i), state, t, kParams).value, before[i - 1])
            << name(static_cast<FactorId>(i));
    }
}

}  // namespace
}  // namespace mda::signals
