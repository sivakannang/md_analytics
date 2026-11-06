#include <gtest/gtest.h>

#include <mda/signals/outcome.hpp>

#include "test_support.hpp"

namespace mda::signals {
namespace {

using namespace test;

constexpr std::int64_t k100 = 100 * core::Price::kScale;

TEST(Outcome, BullishUpIsAHit) {
    const auto result =
        outcome(Label::Bullish, k100, 1001 * core::Price::kScale / 10, 5);  // +10 bp
    EXPECT_EQ(result.return_bp, 10);
    EXPECT_EQ(result.hit, true);
}

TEST(Outcome, BearishUpIsAMiss) {
    EXPECT_EQ(outcome(Label::Bearish, k100, 1001 * core::Price::kScale / 10, 5).hit, false);
    EXPECT_EQ(outcome(Label::Bearish, k100, 999 * core::Price::kScale / 10, 5).hit, true);
}

TEST(Outcome, FlatAndNeutralAreNotCounted) {
    const std::int64_t plus_4bp = k100 + 4 * core::Price::kScale / 100;
    const auto flat = outcome(Label::Bullish, k100, plus_4bp, 5);
    EXPECT_EQ(flat.return_bp, 4);
    EXPECT_FALSE(flat.hit.has_value());
    const auto neutral = outcome(Label::Neutral, k100, 2 * k100, 5);
    EXPECT_EQ(neutral.return_bp, 10'000);
    EXPECT_FALSE(neutral.hit.has_value());
}

TEST(Outcome, UnknownSpotHasNoReturn) {
    const auto result = outcome(Label::Bullish, 0, k100, 5);
    EXPECT_EQ(result.spot_after_raw, k100);
    EXPECT_FALSE(result.return_bp.has_value());
    EXPECT_FALSE(result.hit.has_value());
}

TEST(DayClosed, AfterTheLastCashMinuteOrALaterDay) {
    const std::int64_t last = kMidnightIst + kCashLastMinute;
    EXPECT_FALSE(day_closed(at_min(0), last - 60));
    EXPECT_TRUE(day_closed(at_min(0), last));
    EXPECT_TRUE(day_closed(at_min(0), kMidnightIst + kSecondsPerDay + 33'300));
}

TEST(SettlePrice, FirstCloseAtOrAfterTheHorizon) {
    const std::vector<SpotClose> day = {
        {at_min(0), 1}, {at_min(29), 2}, {at_min(31), 3}, {at_min(40), 4}};
    EXPECT_EQ(settle_price(day, at_min(0), 30, false), 3);  // minute 30 missing: the next bar
    EXPECT_EQ(settle_price(day, at_min(0), 29, false), 2);
    EXPECT_FALSE(settle_price(day, at_min(20), 30, false).has_value());  // not yet
    EXPECT_EQ(settle_price(day, at_min(20), 30, true), 4);               // day over: its last close
    EXPECT_FALSE(settle_price({}, at_min(20), 30, true).has_value());
}

}  // namespace
}  // namespace mda::signals
