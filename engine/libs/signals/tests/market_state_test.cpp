#include <gtest/gtest.h>

#include "test_support.hpp"

namespace mda::signals {
namespace {

using namespace test;

TEST(IstDay, UsesIndianCalendarDay) {
    EXPECT_EQ(ist_day(kMidnightIst), kToday);
    EXPECT_EQ(ist_day(kMidnightIst - 1), kToday - 1);
    EXPECT_EQ(ist_day_start(kOpen), kMidnightIst);
}

TEST(Series, AtSeesOnlyTheWindow) {
    Series<int> series;
    series.add(30, 3);
    series.add(10, 1);
    series.add(20, 2);
    ASSERT_NE(series.at(25, 0), nullptr);
    EXPECT_EQ(*series.at(25, 0), 2);
    EXPECT_EQ(series.at(25, 21), nullptr);  // before `from`
    EXPECT_EQ(series.at(5, 0), nullptr);    // nothing yet
    EXPECT_EQ(series.between(10, 30).size(), 3U);
    EXPECT_TRUE(series.between(31, 40).empty());
}

TEST(Series, SameMinuteReplaces) {
    Series<int> series;
    series.add(10, 1);
    series.add(10, 5);
    EXPECT_EQ(series.between(0, 100).size(), 1U);
    EXPECT_EQ(*series.at(10, 0), 5);
}

TEST(Series, PruneDropsOlderPoints) {
    Series<int> series;
    series.add(10, 1);
    series.add(20, 2);
    series.prune(15);
    EXPECT_EQ(series.at(15, 0), nullptr);
    EXPECT_EQ(*series.at(20, 0), 2);
}

TEST(MarketState, RoutesBarsByRole) {
    auto state = make_state();
    state.add_option({.id = core::InstrumentId{100}, .expiry_days = kToday, .strike = 1000, .call = true});
    state.add_sector(core::InstrumentId{200}, 50);

    state.on_bar(bar({.id = kSpot, .minute = at_min(0), .close = 1000}));
    state.on_bar(bar({.id = kFuture, .minute = at_min(0), .close = 1005, .oi = 7, .volume = 3}));
    state.on_bar(bar({.id = core::InstrumentId{100}, .minute = at_min(0), .close = 10, .oi = 40}));
    state.on_bar(bar({.id = core::InstrumentId{200}, .minute = at_min(0), .close = 51}));

    const auto t = at_min(0);
    EXPECT_DOUBLE_EQ(*state.spot().at(t, kMidnightIst), 1000);
    EXPECT_EQ(state.future().at(t, kMidnightIst)->oi, 7);
    EXPECT_EQ(*state.options()[0].oi.at(t, kMidnightIst), 40);
    EXPECT_DOUBLE_EQ(*state.sectors()[0].close.at(t, kMidnightIst), 51);
}

TEST(MarketState, IgnoresFormingUnknownAndOiLessBars) {
    auto state = make_state();
    state.add_option({.id = core::InstrumentId{100}, .expiry_days = kToday, .strike = 1000, .call = true});

    state.on_bar(bar({.id = kSpot, .minute = at_min(0), .close = 1000, .forming = true}));
    state.on_bar(bar({.id = core::InstrumentId{999}, .minute = at_min(0), .close = 1}));
    state.on_bar(bar({.id = core::InstrumentId{100}, .minute = at_min(0), .close = 10, .oi = 0}));

    EXPECT_TRUE(state.spot().empty());
    EXPECT_TRUE(state.options()[0].oi.empty());
}

TEST(MarketState, NewFutureStartsAfresh) {
    auto state = make_state();
    state.on_bar(bar({.id = kFuture, .minute = at_min(0), .close = 1005, .oi = 7}));
    state.set_future(core::InstrumentId{3});
    EXPECT_TRUE(state.future().empty());
    state.on_bar(bar({.id = kFuture, .minute = at_min(1), .close = 1005, .oi = 7}));
    EXPECT_TRUE(state.future().empty());  // the old future is no longer routed
    state.on_bar(bar({.id = core::InstrumentId{3}, .minute = at_min(1), .close = 1010, .oi = 9}));
    EXPECT_FALSE(state.future().empty());
}

TEST(MarketState, NearestExpiryNeedsATodaySummaryByT) {
    auto state = make_state();
    const auto t = at_min(10);
    state.on_summary(summary({.expiry_days = kToday - 1, .pcr = 1}), t);   // expired
    state.on_summary(summary({.expiry_days = kToday + 7, .pcr = 1}), t);
    state.on_summary(summary({.expiry_days = kToday + 2, .pcr = 1}), t + 60);  // after t
    ASSERT_TRUE(state.nearest_expiry(t).has_value());
    EXPECT_EQ(*state.nearest_expiry(t), kToday + 7);
    EXPECT_EQ(*state.nearest_expiry(t + 60), kToday + 2);
}

TEST(MarketState, SummariesOfOtherSpotsIgnored) {
    auto state = make_state();
    auto other = summary({.pcr = 1});
    other.header.instrument_id = core::InstrumentId{42};
    state.on_summary(other, at_min(0));
    EXPECT_FALSE(state.nearest_expiry(at_min(0)).has_value());
}

TEST(MarketState, StrikesAreDistinctAndSorted) {
    auto state = make_state();
    state.add_option({.id = core::InstrumentId{100}, .expiry_days = kToday, .strike = 1100, .call = true});
    state.add_option({.id = core::InstrumentId{101}, .expiry_days = kToday, .strike = 1000, .call = true});
    state.add_option({.id = core::InstrumentId{102}, .expiry_days = kToday, .strike = 1000, .call = false});
    state.add_option({.id = core::InstrumentId{103}, .expiry_days = kToday + 7, .strike = 900, .call = false});
    EXPECT_EQ(state.strikes(kToday), (std::vector<double>{1000, 1100}));
}

}  // namespace
}  // namespace mda::signals
