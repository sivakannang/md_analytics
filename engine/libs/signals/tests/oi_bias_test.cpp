#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

#include <mda/signals/oi_bias.hpp>
#include <mda/signals/strategies.hpp>

#include "test_support.hpp"

namespace mda::signals {
namespace {

using namespace test;

// Future and PCR both moving one way; the other factors have no data. direction = ±1.
MarketState trending(int direction) {
    auto state = make_state();
    const double d = direction;
    state.on_bar(
        bar({.id = kFuture, .minute = at_min(15), .close = 100, .oi = 1000, .volume = 10}));
    state.on_bar(
        bar({.id = kFuture, .minute = at_min(30), .close = 100 + d, .oi = 1100, .volume = 10}));
    state.on_summary(summary({.pcr = 1.0 - 0.05 * d}), at_min(0));
    state.on_summary(summary({.pcr = 1.0 + 0.05 * d}), at_min(30));
    return state;
}

// F1 ± 20, F4 ± 15·tanh(…), F3 ± 15·tanh(1).
int expected_score(int direction) {
    const double d = direction;
    const double close = 100 + d;
    const double vwap = (100 + close) / 2;
    const double total = 20 * d + 15 * std::tanh((close - vwap) / vwap / 0.002) + 15 * std::tanh(d);
    return static_cast<int>(std::lround(total));
}

TEST(OiBiasV1, EmptyStateIsNeutralWithEveryFactorListed) {
    const auto signal = OiBiasV1{}.evaluate(make_state(), at_min(30));
    EXPECT_EQ(signal.score, 0);
    EXPECT_EQ(signal.label, Label::Neutral);
    ASSERT_EQ(signal.factors.size(), kFactorCount);
    for (const auto& factor : signal.factors) {
        EXPECT_EQ(factor.status, FactorStatus::Absent) << name(factor.id);
        EXPECT_DOUBLE_EQ(factor.points, 0);
    }
}

TEST(OiBiasV1, BullishTrend) {
    const auto signal = OiBiasV1{}.evaluate(trending(+1), at_min(30));
    EXPECT_EQ(signal.score, expected_score(+1));
    EXPECT_GE(signal.score, 25);
    EXPECT_EQ(signal.label, Label::Bullish);
    EXPECT_EQ(signal.spot, kSpot);
    EXPECT_EQ(signal.minute, at_min(30));
    EXPECT_EQ(signal.horizon_min, 30);
    EXPECT_DOUBLE_EQ(signal.factors[0].points, 20);                    // futures long buildup
    EXPECT_EQ(signal.factors[1].status, FactorStatus::Absent);         // no options
    EXPECT_EQ(signal.factors[5].status, FactorStatus::NotApplicable);  // expiry a week out
}

TEST(OiBiasV1, CandidateVixFactorIsScoredButWeighsNothing) {
    auto state = trending(+1);
    state.set_vix(core::InstrumentId{300});
    state.on_bar(bar({.id = core::InstrumentId{300}, .minute = at_min(0), .close = 14}));
    state.on_bar(
        bar({.id = core::InstrumentId{300}, .minute = at_min(30), .close = 15}));  // +7 %: bearish
    const auto signal = OiBiasV1{}.evaluate(state, at_min(30));
    EXPECT_EQ(signal.score, expected_score(+1));
    const auto& vix = signal.factors[static_cast<std::size_t>(FactorId::VixChange) - 1];
    EXPECT_EQ(vix.status, FactorStatus::Present);
    EXPECT_LT(vix.value, -0.9);
    EXPECT_DOUBLE_EQ(vix.points, 0);
}

TEST(OiBiasV1, BearishTrend) {
    const auto signal = OiBiasV1{}.evaluate(trending(-1), at_min(30));
    EXPECT_EQ(signal.score, expected_score(-1));
    EXPECT_EQ(signal.label, Label::Bearish);
}

TEST(OiBiasV1, ThresholdsFromConfig) {
    OiBiasConfig config;
    config.bullish = 60;
    EXPECT_EQ(OiBiasV1{config}.evaluate(trending(+1), at_min(30)).label, Label::Neutral);
}

TEST(OiBiasV1, ScoreIsClamped) {
    OiBiasConfig config;
    config.weights = {150, 0, 0, 0, 0, 0, 0};
    const auto signal = OiBiasV1{config}.evaluate(trending(-1), at_min(30));
    EXPECT_EQ(signal.score, -100);
    EXPECT_EQ(signal.label, Label::Bearish);
}

TEST(OiBiasV1, MadeFromConfig) {
    const auto strategy = make_strategy("oi-bias-v1", 15, 30, -30);
    ASSERT_NE(strategy, nullptr);
    EXPECT_EQ(strategy->horizon_minutes(), 15);
    EXPECT_EQ(make_strategy("nope", 30, 25, -25), nullptr);
}

TEST(OiBiasV1, Names) {
    EXPECT_EQ(name(StrategyId::OiBiasV1), "oi-bias-v1");
    EXPECT_EQ(name(Label::Bullish), "bullish");
    EXPECT_EQ(name(FactorId::AtmOiShift), "atm_oi_shift");
}

}  // namespace
}  // namespace mda::signals
