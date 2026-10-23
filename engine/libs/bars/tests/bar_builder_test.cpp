#include <vector>

#include <gtest/gtest.h>

#include <mda/bars/bar_builder.hpp>

namespace mda::bars {
namespace {

using messages::MarketSnapshot;

constexpr std::int64_t kMidnightIst = 1'792'607'400;  // 2026-10-22 00:00 IST

/// Unix seconds of hh:mm:ss IST on 2026-10-22.
std::int64_t ist(int h, int m, int s = 0) {
    return kMidnightIst + h * 3600 + m * 60 + s;
}

core::Price rupees(double value) {
    return core::Price::from_raw(static_cast<std::int64_t>(value * core::Price::kScale));
}

struct Tick {
    std::uint32_t id = 7;
    std::int64_t at = 0;  ///< exchange time; 0 = none
    double ltp = 0;
    std::int64_t volume = -1;
    std::int64_t oi = -1;
    std::int64_t received = 0;
};

MarketSnapshot snap(const Tick& t) {
    MarketSnapshot s{};
    s.header = messages::make_header<MarketSnapshot>(core::InstrumentId(t.id), 1);
    if (t.ltp > 0) {
        s.fields |= messages::kFieldLtp;
        s.ltp = rupees(t.ltp);
    }
    if (t.volume >= 0) {
        s.fields |= messages::kFieldVolume;
        s.volume = core::Quantity(t.volume);
    }
    if (t.oi >= 0) {
        s.fields |= messages::kFieldOpenInterest;
        s.oi = core::Quantity(t.oi);
    }
    if (t.at != 0) {
        s.fields |= messages::kFieldExchangeTime;
        s.exchange_ts = core::Timestamp::from_seconds(t.at);
    }
    s.receive_ts = core::Timestamp::from_seconds(t.received != 0 ? t.received : t.at + 1);
    return s;
}

class BarBuilderTest : public ::testing::Test {
protected:
    BarBuilder builder_{{}, [this](const Bar& bar) { closed_.push_back(bar); }};
    std::vector<Bar> closed_;
};

TEST_F(BarBuilderTest, BuildsOhlcAndVolumeFromExchangeTime) {
    builder_.on_snapshot(snap({.at = ist(9, 15, 1), .ltp = 100, .volume = 1000}));
    builder_.on_snapshot(snap({.at = ist(9, 15, 20), .ltp = 104, .volume = 1150}));
    builder_.on_snapshot(snap({.at = ist(9, 15, 59), .ltp = 99, .volume = 1200}));
    EXPECT_TRUE(closed_.empty());
    builder_.on_snapshot(snap({.at = ist(9, 16, 2), .ltp = 101, .volume = 1500}));

    ASSERT_EQ(closed_.size(), 1u);
    const Bar& bar = closed_[0];
    EXPECT_EQ(bar.start, ist(9, 15));
    EXPECT_EQ(bar.open, rupees(100));
    EXPECT_EQ(bar.high, rupees(104));
    EXPECT_EQ(bar.low, rupees(99));
    EXPECT_EQ(bar.close, rupees(99));
    EXPECT_EQ(bar.volume.value(), 200);  // first tick sets the baseline
    EXPECT_EQ(bar.trades, 2u);
    EXPECT_EQ(bar.header.flags, 0);

    builder_.close_due(ist(9, 17, 2));
    ASSERT_EQ(closed_.size(), 2u);
    EXPECT_EQ(closed_[1].volume.value(), 300);
}

TEST_F(BarBuilderTest, PreOpenMakesNoBarButSetsTheVolumeBaseline) {
    builder_.on_snapshot(snap({.at = ist(9, 7, 30), .ltp = 100, .volume = 5000}));  // pre-open
    builder_.on_snapshot(snap({.at = ist(9, 15, 3), .ltp = 101, .volume = 5600}));
    builder_.close_due(ist(9, 16, 5));
    ASSERT_EQ(closed_.size(), 1u);
    EXPECT_EQ(closed_[0].start, ist(9, 15));
    EXPECT_EQ(closed_[0].volume.value(), 600);

    builder_.on_snapshot(
        snap({.at = ist(15, 30, 1), .ltp = 102, .volume = 7000}));  // after the close
    builder_.close_due(ist(15, 32));
    EXPECT_EQ(closed_.size(), 1u);
}

TEST_F(BarBuilderTest, ClosesOnTheTimerAfterTheDelay) {
    builder_.on_snapshot(snap({.at = ist(10, 0, 10), .ltp = 50}));
    builder_.close_due(ist(10, 1, 1));  // 1 s after the minute: too early
    EXPECT_TRUE(closed_.empty());
    builder_.close_due(ist(10, 1, 2));
    ASSERT_EQ(closed_.size(), 1u);
    builder_.close_due(ist(10, 5));  // nothing more: minutes without ticks make no bar
    EXPECT_EQ(closed_.size(), 1u);
}

TEST_F(BarBuilderTest, CountsOiUpdatesAndKeepsOiOnlyMinutes) {
    builder_.on_snapshot(snap({.at = ist(10, 0, 5), .ltp = 200, .volume = 100, .oi = 50'000}));
    builder_.on_snapshot(snap({.at = ist(10, 0, 40), .ltp = 201, .volume = 160, .oi = 52'000}));
    // 10:01: OI moves, no trade (same price and volume).
    builder_.on_snapshot(snap({.at = ist(10, 1, 30), .ltp = 201, .volume = 160, .oi = 51'500}));
    builder_.close_due(ist(10, 2, 5));

    ASSERT_EQ(closed_.size(), 2u);
    EXPECT_EQ(closed_[0].oi_open.value(), 50'000);
    EXPECT_EQ(closed_[0].oi_high.value(), 52'000);
    EXPECT_EQ(closed_[0].oi_close.value(), 52'000);
    EXPECT_EQ(closed_[0].oi_updates, 1u);

    const Bar& oi_only = closed_[1];
    EXPECT_EQ(oi_only.trades, 0u);
    EXPECT_EQ(oi_only.volume.value(), 0);
    EXPECT_EQ(oi_only.close, rupees(201));
    EXPECT_EQ(oi_only.oi_open.value(), 52'000);  // OI at the minute's start
    EXPECT_EQ(oi_only.oi_low.value(), 51'500);
    EXPECT_EQ(oi_only.oi_updates, 1u);
}

TEST_F(BarBuilderTest, RecordsWhenOiLastChangedByReceiveTime) {
    builder_.on_snapshot(
        snap({.at = ist(10, 0, 5), .ltp = 200, .oi = 50'000}));  // first OI: no change
    builder_.on_snapshot(snap({.at = ist(10, 0, 10), .ltp = 201}));
    builder_.close_due(ist(10, 1, 5));
    builder_.on_snapshot(
        snap({.at = ist(10, 2, 10), .ltp = 201, .oi = 51'000, .received = ist(10, 2, 20)}));
    builder_.on_snapshot(snap({.at = ist(10, 2, 30), .ltp = 202, .oi = 51'000}));  // unchanged OI
    builder_.on_snapshot(
        snap({.at = ist(10, 2, 59), .ltp = 202, .oi = 52'000, .received = ist(10, 3, 1)}));
    builder_.close_due(ist(10, 3, 5));

    ASSERT_EQ(closed_.size(), 2u);
    EXPECT_EQ(closed_[0].oi_update_offset, kNoOiUpdateTime);
    EXPECT_EQ(closed_[1].oi_updates, 2u);
    EXPECT_EQ(closed_[1].oi_update_offset, 61);  // received just after its minute ended
}

TEST_F(BarBuilderTest, IgnoresLateTicksAndKeepsInstrumentsApart) {
    builder_.on_snapshot(snap({.id = 7, .at = ist(11, 0, 5), .ltp = 10}));
    builder_.on_snapshot(snap({.id = 8, .at = ist(11, 0, 6), .ltp = 20}));
    builder_.on_snapshot(snap({.id = 7, .at = ist(11, 1, 1), .ltp = 11}));
    builder_.on_snapshot(snap({.id = 7, .at = ist(11, 0, 59), .ltp = 99}));  // late: dropped
    builder_.close_due(ist(11, 2, 5));
    ASSERT_EQ(closed_.size(), 3u);
    for (const Bar& bar : closed_) {
        EXPECT_NE(bar.high, rupees(99));
    }
    EXPECT_EQ(builder_.instruments(), 2u);
}

TEST_F(BarBuilderTest, FallsBackToReceiveTimeAndReportsFormingBars) {
    builder_.on_snapshot(snap({.at = 0, .ltp = 75, .received = ist(12, 30, 15)}));
    const auto forming = builder_.forming();
    ASSERT_EQ(forming.size(), 1u);
    EXPECT_EQ(forming[0].start, ist(12, 30));
    EXPECT_EQ(forming[0].header.flags, kBarForming);
}

TEST_F(BarBuilderTest, SeededCloseLetsAnOiOnlyTickMakeABar) {
    Bar last{};
    last.header = messages::make_header<Bar>(core::InstrumentId(7), 0);
    last.close = rupees(300);
    last.oi_close = core::Quantity(10'000);
    builder_.seed(last);
    builder_.on_snapshot(snap({.at = ist(13, 0, 5), .oi = 10'500}));  // no price in the tick
    builder_.close_due(ist(13, 1, 5));
    ASSERT_EQ(closed_.size(), 1u);
    EXPECT_EQ(closed_[0].close, rupees(300));
    EXPECT_EQ(closed_[0].oi_updates, 1u);
}

TEST(BarBuilderSessions, DerivativesTradeUntil1540AndCashUntil1530) {
    std::vector<Bar> closed;
    BuilderOptions options;
    options.is_derivative = [](core::InstrumentId id) { return id.value() == 8; };
    BarBuilder builder(std::move(options), [&](const Bar& bar) { closed.push_back(bar); });
    for (const std::uint32_t id : {7u, 8u}) {
        builder.on_snapshot(snap({.id = id, .at = ist(15, 29, 30), .ltp = 100, .volume = 10}));
        builder.on_snapshot(snap({.id = id, .at = ist(15, 35, 1), .ltp = 101, .volume = 20}));
        builder.on_snapshot(snap({.id = id, .at = ist(15, 40, 1), .ltp = 102, .volume = 30}));
    }
    builder.close_due(ist(15, 45));
    ASSERT_EQ(closed.size(), 3u);  // cash: 15:29; future: 15:29 and 15:35, not 15:40
    EXPECT_EQ(closed[0].header.instrument_id.value(), 7u);
    EXPECT_EQ(closed[2].header.instrument_id.value(), 8u);
    EXPECT_EQ(closed[2].start, ist(15, 35));
    EXPECT_EQ(closed[2].volume.value(), 10);
}

TEST_F(BarBuilderTest, ClosingAuctionVolumeDropIsNotVolume) {
    // Continuous trading to 15:15; during the auction Kite's volume field shows the matched
    // quantity, then the day total (+ the auction's 300) after the match.
    builder_.on_snapshot(snap({.at = ist(15, 14, 10), .ltp = 100, .volume = 10'000}));
    builder_.on_snapshot(snap({.at = ist(15, 14, 50), .ltp = 100.5, .volume = 10'500}));
    builder_.on_snapshot(snap({.at = ist(15, 20, 5), .ltp = 100.5, .volume = 120}));
    builder_.on_snapshot(snap({.at = ist(15, 24, 5), .ltp = 100.5, .volume = 280}));
    builder_.on_snapshot(snap({.at = ist(15, 28, 54), .ltp = 100.8, .volume = 10'800}));
    builder_.close_due(ist(15, 31));
    ASSERT_EQ(closed_.size(), 4u);
    EXPECT_EQ(closed_[0].volume.value(), 500);
    EXPECT_EQ(closed_[1].volume.value(), 0);  // 15:20
    EXPECT_EQ(closed_[2].volume.value(), 0);  // 15:24
    EXPECT_EQ(closed_[3].volume.value(), 300);
    EXPECT_EQ(closed_[3].close, rupees(100.8));
}

TEST_F(BarBuilderTest, SeededDayVolumeIsTheBaselineAfterARestart) {
    Bar last{};
    last.header = messages::make_header<Bar>(core::InstrumentId(7), 0);
    last.start = ist(15, 14);
    last.close = rupees(100.5);
    builder_.seed(last, 10'500);
    // Restarted during the auction: the first tick shows the matched quantity.
    builder_.on_snapshot(snap({.at = ist(15, 22, 5), .ltp = 100.5, .volume = 280}));
    builder_.on_snapshot(snap({.at = ist(15, 28, 54), .ltp = 100.8, .volume = 10'800}));
    builder_.close_due(ist(15, 31));
    ASSERT_EQ(closed_.size(), 2u);
    EXPECT_EQ(closed_[1].volume.value(), 300);

    // Restarted mid-session after missing trades: they don't land in the first bar.
    Bar other = last;
    other.header = messages::make_header<Bar>(core::InstrumentId(9), 0);
    builder_.seed(other, 5'000);
    builder_.on_snapshot(snap({.id = 9, .at = ist(15, 25, 1), .ltp = 100, .volume = 7'000}));
    builder_.on_snapshot(snap({.id = 9, .at = ist(15, 25, 9), .ltp = 101, .volume = 7'100}));
    builder_.close_due(ist(15, 31));
    EXPECT_EQ(closed_.back().volume.value(), 100);
}

TEST(IstMinuteTest, ConvertsToIndianTime) {
    EXPECT_EQ(ist_minute_of_day(ist(9, 15)), 9 * 60 + 15);
    EXPECT_EQ(ist_minute_of_day(ist(0, 0)), 0);
    EXPECT_EQ(ist_minute_of_day(ist(23, 59, 59)), 23 * 60 + 59);
}

}  // namespace
}  // namespace mda::bars
