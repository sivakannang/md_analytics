// F9, the near-month future's signed volume (docs/design/signals.md §13): the snapshot
// classifier, the per-minute totals in MarketState and the factor.

#include <cmath>

#include <gtest/gtest.h>

#include <mda/signals/factors.hpp>

#include "test_support.hpp"

namespace mda::signals {
namespace {

using namespace test;

const FactorParams kParams{};

messages::MarketSnapshot snapshot(std::int64_t seconds_after_open, double ltp, std::int64_t volume,
                                  core::InstrumentId id = kFuture) {
    messages::MarketSnapshot s{};
    s.header.instrument_id = id;
    s.fields = messages::kFieldLtp | messages::kFieldVolume | messages::kFieldExchangeTime;
    s.exchange_ts = core::Timestamp::from_seconds(kOpen + seconds_after_open);
    s.ltp = price(ltp);
    s.volume = core::Quantity{volume};
    return s;
}

messages::DepthSnapshot depth(double bid, double ask, core::InstrumentId id = kFuture) {
    messages::DepthSnapshot d{};
    d.header.instrument_id = id;
    d.bids[0] = {.price = price(bid), .quantity = 75, .orders = 1};
    d.asks[0] = {.price = price(ask), .quantity = 75, .orders = 1};
    return d;
}

std::int64_t raw(double value) {
    return price(value).raw();
}

TEST(ClassifyTrade, QuoteRuleFirst) {
    const auto bid = raw(100.0);
    const auto ask = raw(100.5);
    EXPECT_EQ(classify_trade(raw(100.5), bid, ask, raw(101), -1),
              Aggressor::Buy);  // at the ask, LTP fell
    EXPECT_EQ(classify_trade(raw(100.6), bid, ask, raw(100), 0),
              Aggressor::Buy);  // through the ask
    EXPECT_EQ(classify_trade(raw(100.0), bid, ask, raw(99), 1),
              Aggressor::Sell);  // at the bid, LTP rose
}

TEST(ClassifyTrade, TickRuleInsideTheSpreadOrWithoutAQuote) {
    const auto bid = raw(100.0);
    const auto ask = raw(100.5);
    EXPECT_EQ(classify_trade(raw(100.25), bid, ask, raw(100.1), 0), Aggressor::Buy);
    EXPECT_EQ(classify_trade(raw(100.25), bid, ask, raw(100.4), 0), Aggressor::Sell);
    EXPECT_EQ(classify_trade(raw(100.25), bid, ask, raw(100.25), -1),
              Aggressor::Sell);  // zero tick: last change
    EXPECT_EQ(classify_trade(raw(100.25), 0, 0, raw(100.25), 0), Aggressor::Unknown);
    EXPECT_EQ(classify_trade(raw(101), 0, 0, raw(100), 0), Aggressor::Buy);
}

TEST(Flow, ClassifiesNewVolumeAgainstThePreviousQuote) {
    auto state = make_state();
    state.on_snapshot(
        snapshot(1, 100.0, 5'000));  // baseline: the day's volume so far isn't counted
    state.on_depth(depth(100.0, 100.5));
    state.on_snapshot(snapshot(2, 100.5, 5'300));  // +300 at the ask: buy
    state.on_depth(depth(100.5, 101.0));           // the quote after that trade
    state.on_snapshot(snapshot(3, 100.5, 5'400));  // +100 at the (new) bid: sell
    state.on_snapshot(snapshot(4, 100.5, 5'400));  // nothing traded
    const auto* minute = state.flow().at(at_min(0), at_min(0));
    ASSERT_NE(minute, nullptr);
    EXPECT_EQ(minute->buy, 300);
    EXPECT_EQ(minute->sell, 100);
    EXPECT_EQ(minute->unclassified, 0);
}

TEST(Flow, MinuteFromExchangeTimeAndOnlyTheFuture) {
    auto state = make_state();
    state.on_snapshot(snapshot(58, 100, 1'000));
    state.on_snapshot(snapshot(61, 101, 1'200));                // next minute, tick up
    state.on_snapshot(snapshot(62, 99, 9'999, kSpot));          // not the future
    state.on_depth(depth(50, 51, kSpot));                       // nor its depth
    state.on_snapshot(snapshot(63, 101, 1'250));                // zero tick after a rise
    EXPECT_EQ(state.flow().at(at_min(0), at_min(0)), nullptr);  // a baseline adds no minute
    EXPECT_EQ(state.flow().at(at_min(1), at_min(1))->buy, 250);
}

TEST(Flow, BaselineResetsAfterAGapAndOnARoll) {
    auto state = make_state();
    state.on_snapshot(snapshot(1, 100, 1'000));
    state.reset_flow_baseline();
    state.on_snapshot(snapshot(2, 101, 50'000));  // lost messages: this volume isn't one second's
    state.on_snapshot(snapshot(3, 102, 50'100));
    EXPECT_EQ(state.flow().at(at_min(0), at_min(0))->buy, 100);
    state.set_future(core::InstrumentId{3});
    EXPECT_TRUE(state.flow().empty());
    state.on_snapshot(snapshot(4, 102, 60'000));  // the old future: ignored
    EXPECT_TRUE(state.flow().empty());
}

// The Wi-Fi was down for 30 minutes: the volume traded meanwhile is a baseline, not a trade.
TEST(Flow, BaselineResetsAfterAFeedOutage) {
    auto state = make_state();
    state.on_snapshot(snapshot(1, 100, 1'000));
    state.on_depth(depth(100, 100.5));
    state.on_snapshot(snapshot(2, 100.5, 1'100));           // +100 at the ask
    state.on_snapshot(snapshot(30 * 60 + 5, 104, 90'000));  // back after the outage
    state.on_depth(depth(104, 104.5));
    state.on_snapshot(snapshot(30 * 60 + 6, 104.5, 90'040));  // +40 at the ask
    EXPECT_EQ(state.flow().at(at_min(0), at_min(0))->buy, 100);
    const auto* after = state.flow().at(at_min(30), at_min(30));
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->buy, 40);
    EXPECT_EQ(after->sell + after->unclassified, 0);
}

// Other instruments' snapshots keep the feed alive while the future is quiet.
TEST(Flow, AQuietFutureIsNoOutage) {
    auto state = make_state();
    state.on_snapshot(snapshot(1, 100, 1'000));
    for (int s = 60; s <= 600; s += 60) {
        state.on_snapshot(snapshot(s, 50, 10 + s, core::InstrumentId{99}));
    }
    state.on_snapshot(snapshot(601, 101, 1'200));  // LTP up, no quote: tick rule
    const auto* minute = state.flow().at(at_min(10), at_min(10));
    ASSERT_NE(minute, nullptr);
    EXPECT_EQ(minute->buy, 200);
}

TEST(Flow, PreOpenAuctionOnlyMovesTheBaseline) {
    auto state = make_state();
    for (int s = -590; s < 0; s += 60) {  // other instruments tick throughout: no outage
        state.on_snapshot(snapshot(s, 50, 10, core::InstrumentId{99}));
    }
    state.on_snapshot(snapshot(-600, 100, 0));       // 09:05, pre-open
    state.on_snapshot(snapshot(-420, 101, 80'000));  // 09:08, the auction match
    state.on_snapshot(snapshot(5, 102, 80'500));     // 09:15:05, trading
    EXPECT_EQ(state.flow().between(at_min(-15), at_min(-1)).size(), 0u);
    EXPECT_EQ(state.flow().at(at_min(0), at_min(0))->buy, 500);
}

// One minute of flow: a baseline at :00 and `buy`/`sell` lots at :10/:20.
void add_minute(MarketState& state, std::int64_t minute, std::int64_t buy, std::int64_t sell,
                std::int64_t& volume) {
    const std::int64_t s = minute * 60;
    state.on_depth(depth(100, 101));
    state.on_snapshot(snapshot(s + 10, 101, volume += buy));  // at the ask
    state.on_depth(depth(100, 101));
    state.on_snapshot(snapshot(s + 20, 100, volume += sell));  // at the bid
}

MarketState with_flow(int minutes, std::int64_t buy, std::int64_t sell) {
    auto state = make_state();
    std::int64_t volume = 1'000;
    state.on_snapshot(snapshot(0, 100, volume));
    for (int m = 0; m < minutes; ++m) {
        add_minute(state, m, buy, sell, volume);
    }
    return state;
}

TEST(VolumeDelta, ImbalanceOverTheWindow) {
    const auto value =
        volume_delta(with_flow(15, 600, 400), at_min(14), kParams);  // (600−400)/1000
    EXPECT_EQ(value.status, FactorStatus::Present);
    EXPECT_NEAR(value.value, std::tanh(0.2 / 0.3), 1e-9);
    EXPECT_NEAR(volume_delta(with_flow(15, 400, 600), at_min(14), kParams).value,
                -std::tanh(0.2 / 0.3), 1e-9);
}

TEST(VolumeDelta, OnlyTheLast15ClosedMinutes) {
    auto state = with_flow(15, 500, 500);  // balanced 09:15–09:29
    std::int64_t volume = 1'000 + 15 * 1'000;
    add_minute(state, 15, 900, 100, volume);  // 09:30, after t
    EXPECT_DOUBLE_EQ(volume_delta(state, at_min(14), kParams).value, 0);
    // At 09:30 the window is 09:16–09:30: 14 balanced minutes and one +800.
    EXPECT_NEAR(volume_delta(state, at_min(15), kParams).value, std::tanh((800.0 / 15'000) / 0.3),
                1e-9);
}

TEST(VolumeDelta, AbsentWithTooFewMinutesOrUnclassifiedVolume) {
    EXPECT_EQ(volume_delta(with_flow(9, 600, 400), at_min(8), kParams).status,
              FactorStatus::Absent);
    EXPECT_EQ(volume_delta(with_flow(10, 600, 400), at_min(9), kParams).status,
              FactorStatus::Present);
    // Snapshots every minute but trades in only 9 of them (a thin future): absent.
    auto thin = with_flow(9, 600, 400);
    for (int m = 9; m < 15; ++m) {
        thin.on_snapshot(snapshot(m * 60 + 30, 100, 1'000 + 9 * 1'000));
    }
    EXPECT_EQ(volume_delta(thin, at_min(14), kParams).status, FactorStatus::Absent);
    // No depth and an unchanged LTP: nothing can be classified.
    auto state = make_state();
    std::int64_t volume = 0;
    for (int m = 0; m < 15; ++m) {
        state.on_snapshot(snapshot(m * 60 + 5, 100, volume += 100));
    }
    EXPECT_EQ(volume_delta(state, at_min(14), kParams).status, FactorStatus::Absent);
}

}  // namespace
}  // namespace mda::signals
