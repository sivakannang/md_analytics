#include <vector>

#include <gtest/gtest.h>

#include <mda/bars/aggregate.hpp>
#include <mda/bars/buildup.hpp>

namespace mda::bars {
namespace {

constexpr std::int64_t kMidnightIst = 1'792'607'400;  // 2026-10-22 00:00 IST
constexpr core::Quantity kLot{75};

std::int64_t ist(int h, int m) {
    return kMidnightIst + h * 3600 + m * 60;
}

/// A minute closing at `close` (raw price units) with OI `oi` and `updates` OI changes.
Bar minute(std::int64_t start, std::int64_t close, std::int64_t oi, std::uint32_t updates,
           BarSource source = BarSource::Live) {
    Bar b{};
    b.start = start;
    b.open = b.high = b.low = b.close = core::Price::from_raw(close);
    b.oi_open = b.oi_high = b.oi_low = b.oi_close = core::Quantity(oi);
    b.oi_updates = updates;
    b.source = source;
    return b;
}

TEST(Buildup, FirstUpdateHasNothingToCompareWith) {
    const std::vector<Bar> bars{minute(ist(10, 0), 100, 1'000'000, 1)};
    EXPECT_EQ(classify_buildup(bars, kLot), std::vector{Buildup::None});
}

TEST(Buildup, NoOiIsNone) {
    const std::vector<Bar> bars{minute(ist(10, 0), 100, 0, 0), minute(ist(10, 1), 110, 0, 0)};
    EXPECT_EQ(classify_buildup(bars, kLot), (std::vector{Buildup::None, Buildup::None}));
}

TEST(Buildup, FourQuadrantsCloseToClose) {
    const std::vector<Bar> bars{
        minute(ist(10, 0), 100, 1'000'000, 1), minute(ist(10, 1), 110, 1'010'000, 1),  // up, up
        minute(ist(10, 2), 105, 1'020'000, 1),                                         // down, up
        minute(ist(10, 3), 108, 1'010'000, 1),                                         // up, down
        minute(ist(10, 4), 101, 1'000'000, 1),                                         // down, down
    };
    EXPECT_EQ(classify_buildup(bars, kLot),
              (std::vector{Buildup::None, Buildup::LongBuildup, Buildup::ShortBuildup,
                           Buildup::ShortCovering, Buildup::LongUnwinding}));
}

TEST(Buildup, UnchangedPriceCountsAsUp) {
    const std::vector<Bar> bars{minute(ist(10, 0), 100, 1'000'000, 1),
                                minute(ist(10, 1), 100, 990'000, 1)};
    EXPECT_EQ(classify_buildup(bars, kLot)[1], Buildup::ShortCovering);
}

// The design's example: OI arrives at 10:03 and 10:06; 10:06 is measured from 10:03.
TEST(Buildup, MeasuresFromLastOiUpdate) {
    const std::vector<Bar> bars{
        minute(ist(10, 3), 100, 1'000'000, 2), minute(ist(10, 4), 120, 1'000'000, 0),
        minute(ist(10, 5), 90, 1'000'000, 0),
        minute(ist(10, 6), 95, 1'010'000, 1),  // price 100 -> 95, OI up
    };
    EXPECT_EQ(classify_buildup(bars, kLot),
              (std::vector{Buildup::None, Buildup::NoOiUpdate, Buildup::NoOiUpdate,
                           Buildup::ShortBuildup}));
}

TEST(Buildup, PointsCarryTheOiChangeTheStateWasMeasuredOn) {
    const std::vector<Bar> bars{
        minute(ist(10, 3), 100, 1'000'000, 2),
        minute(ist(10, 4), 120, 1'000'000, 0),  // no update: no change
        minute(ist(10, 6), 95, 1'010'000, 1),   // measured from 10:03
    };
    const auto points = classify_buildup_points(bars, kLot);
    ASSERT_EQ(points.size(), 3u);
    EXPECT_FALSE(points[0].oi_change.has_value());
    EXPECT_FALSE(points[1].oi_change.has_value());
    EXPECT_EQ(points[2].oi_change, std::optional<std::int64_t>(10'000));
    EXPECT_EQ(points[2].state, Buildup::ShortBuildup);
}

TEST(Buildup, NeutralBelowFractionOfOi) {
    // 0.1% of 1,000,000 = 1,000 beats 1 lot (75).
    const std::vector<Bar> bars{
        minute(ist(10, 0), 100, 1'000'000, 1),
        minute(ist(10, 1), 110, 1'000'999, 1),
        minute(ist(10, 2), 120, 1'003'000, 1),
    };
    EXPECT_EQ(classify_buildup(bars, kLot),
              (std::vector{Buildup::None, Buildup::Neutral, Buildup::LongBuildup}));
}

TEST(Buildup, NeutralBelowOneLot) {
    // 0.1% of 10,000 = 10, so the lot (75) is the threshold.
    const std::vector<Bar> bars{
        minute(ist(10, 0), 100, 10'000, 1),
        minute(ist(10, 1), 110, 10'074, 1),
        minute(ist(10, 2), 120, 10'149, 1),
    };
    EXPECT_EQ(classify_buildup(bars, kLot),
              (std::vector{Buildup::None, Buildup::Neutral, Buildup::LongBuildup}));
}

TEST(Buildup, UpdateWithoutNetChangeIsNeutral) {
    const std::vector<Bar> bars{minute(ist(10, 0), 100, 1'000'000, 1),
                                minute(ist(10, 1), 110, 1'000'000, 3)};
    EXPECT_EQ(classify_buildup(bars, core::Quantity{0}, {.oi_fraction = 0, .lots = 0})[1],
              Buildup::Neutral);
}

TEST(Buildup, FormingBarIsPeekedAndNotAReference) {
    std::vector<Bar> bars{
        minute(ist(10, 0), 100, 1'000'000, 1),
        minute(ist(10, 1), 110, 1'010'000, 1),
    };
    bars[1].header.flags = kBarForming;
    EXPECT_EQ(classify_buildup(bars, kLot)[1], Buildup::LongBuildup);

    BuildupClassifier classifier(kLot);
    classifier.on_close(bars[0]);
    EXPECT_EQ(classifier.peek(bars[1]), Buildup::LongBuildup);
    // Still measured from 10:00, not from the peeked bar.
    EXPECT_EQ(classifier.on_close(minute(ist(10, 1), 90, 1'010'000, 1)), Buildup::ShortBuildup);
}

TEST(Buildup, StreamingMatchesBatch) {
    const std::vector<Bar> bars{
        minute(ist(10, 0), 100, 1'000'000, 1), minute(ist(10, 1), 110, 1'010'000, 0),
        minute(ist(10, 2), 105, 1'020'000, 1), minute(ist(10, 3), 108, 1'020'500, 1),
        minute(ist(10, 4), 101, 1'000'000, 1),
    };
    BuildupClassifier classifier(kLot);
    std::vector<Buildup> streamed;
    for (const Bar& bar : bars) {
        streamed.push_back(classifier.on_close(bar));
    }
    EXPECT_EQ(streamed, classify_buildup(bars, kLot));
}

TEST(Buildup, BackfillCountsOiChanges) {
    std::vector<Bar> minutes{
        minute(ist(10, 0), 100, 1'000'000, 0, BarSource::Kite),
        minute(ist(10, 1), 100, 1'000'000, 0, BarSource::Kite),
        minute(ist(10, 2), 100, 1'010'000, 0, BarSource::Kite),
        minute(ist(10, 3), 100, 1'010'000, 4, BarSource::Live),  // live counts are kept
        minute(ist(10, 4), 100, 0, 0, BarSource::Kite),          // no OI in the candle
    };
    mark_backfill_oi_updates(minutes, core::Quantity{990'000});
    std::vector<std::uint32_t> updates;
    for (const Bar& bar : minutes) {
        updates.push_back(bar.oi_updates);
    }
    EXPECT_EQ(updates, (std::vector<std::uint32_t>{1, 0, 1, 4, 0}));
}

TEST(Buildup, BackfillWithUnknownPreviousOi) {
    std::vector<Bar> minutes{minute(ist(10, 0), 100, 1'000'000, 0, BarSource::Kite)};
    mark_backfill_oi_updates(minutes);
    EXPECT_EQ(minutes[0].oi_updates, 0u);
}

TEST(Buildup, AggregatedBarsUseSummedUpdates) {
    // 3m buckets from 10:00 (09:15 + 45 min): updates only in the first bucket's middle
    // minute and the third bucket.
    std::vector<Bar> minutes{
        minute(ist(10, 0), 100, 1'000'000, 0), minute(ist(10, 1), 101, 1'005'000, 1),
        minute(ist(10, 2), 102, 1'005'000, 0), minute(ist(10, 3), 98, 1'005'000, 0),
        minute(ist(10, 4), 97, 1'005'000, 0),  minute(ist(10, 5), 96, 1'005'000, 0),
        minute(ist(10, 6), 95, 1'005'000, 0),  minute(ist(10, 7), 94, 1'015'000, 1),
        minute(ist(10, 8), 93, 1'015'000, 0),
    };
    const std::vector<Bar> bars = aggregate(minutes, Interval{.minutes = 3});
    ASSERT_EQ(bars.size(), 3u);
    // 10:06 bucket closes at 93 with OI 1,015,000, measured from 10:00's close 102 / 1,005,000.
    EXPECT_EQ(classify_buildup(bars, kLot),
              (std::vector{Buildup::None, Buildup::NoOiUpdate, Buildup::ShortBuildup}));
}

}  // namespace
}  // namespace mda::bars
