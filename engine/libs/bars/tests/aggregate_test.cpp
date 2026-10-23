#include <vector>

#include <gtest/gtest.h>

#include <mda/bars/aggregate.hpp>

namespace mda::bars {
namespace {

constexpr std::int64_t kMidnightIst = 1'792'607'400;  // 2026-10-22 00:00 IST

std::int64_t ist(int h, int m) {
    return kMidnightIst + h * 3600 + m * 60;
}

Bar minute(std::int64_t start, std::int64_t o, std::int64_t h, std::int64_t l, std::int64_t c,
           std::int64_t volume, std::int64_t oi = 0, BarSource source = BarSource::Live) {
    Bar b{};
    b.start = start;
    b.open = core::Price::from_raw(o);
    b.high = core::Price::from_raw(h);
    b.low = core::Price::from_raw(l);
    b.close = core::Price::from_raw(c);
    b.volume = core::Quantity(volume);
    if (oi > 0) {
        b.oi_open = b.oi_high = b.oi_low = b.oi_close = core::Quantity(oi);
        b.oi_updates = 1;
    }
    b.source = source;
    return b;
}

TEST(IntervalTest, ParsesKnownIntervals) {
    EXPECT_EQ(parse_interval("5m"), (Interval{.minutes = 5}));
    EXPECT_EQ(parse_interval("1h"), (Interval{.minutes = 60}));
    EXPECT_TRUE(parse_interval("1d")->daily);
    EXPECT_FALSE(parse_interval("7m").has_value());
    EXPECT_FALSE(parse_interval("").has_value());
}

TEST(IntervalTest, BucketsAlignToTheSessionStartLikeKite) {
    EXPECT_EQ(bucket_start(ist(9, 17), {.minutes = 5}), ist(9, 15));
    EXPECT_EQ(bucket_start(ist(9, 20), {.minutes = 5}), ist(9, 20));
    EXPECT_EQ(bucket_start(ist(10, 14), {.minutes = 60}), ist(9, 15));
    EXPECT_EQ(bucket_start(ist(15, 29), {.minutes = 60}), ist(15, 15));
    EXPECT_EQ(bucket_start(ist(15, 29), {.minutes = 1440, .daily = true}), kMidnightIst);
    EXPECT_EQ(bucket_start(ist(9, 16), {.minutes = 15}), ist(9, 15));
}

TEST(AggregateTest, MergesMinutesIntoBuckets) {
    const std::vector<Bar> minutes{
        minute(ist(9, 15), 100, 105, 99, 104, 10, 1000),
        minute(ist(9, 16), 104, 108, 103, 107, 20, 1200),
        minute(ist(9, 19), 107, 107, 95, 96, 5, 900, BarSource::Kite),
        minute(ist(9, 20), 96, 98, 96, 97, 7),  // no OI in this minute
    };
    const auto bars = aggregate(minutes, {.minutes = 5});
    ASSERT_EQ(bars.size(), 2u);
    const Bar& first = bars[0];
    EXPECT_EQ(first.start, ist(9, 15));
    EXPECT_EQ(first.open.raw(), 100);
    EXPECT_EQ(first.high.raw(), 108);
    EXPECT_EQ(first.low.raw(), 95);
    EXPECT_EQ(first.close.raw(), 96);
    EXPECT_EQ(first.volume.value(), 35);
    EXPECT_EQ(first.oi_open.value(), 1000);
    EXPECT_EQ(first.oi_high.value(), 1200);
    EXPECT_EQ(first.oi_low.value(), 900);
    EXPECT_EQ(first.oi_close.value(), 900);
    EXPECT_EQ(first.oi_updates, 3u);
    EXPECT_EQ(first.source, BarSource::Live);  // any live minute makes the bucket live

    EXPECT_EQ(bars[1].oi_close.value(), 0);
    EXPECT_EQ(bars[1].volume.value(), 7);
}

TEST(AggregateTest, DailyBarsSpanTheIstDay) {
    const std::vector<Bar> minutes{
        minute(ist(9, 15) - 86'400, 50, 55, 49, 54, 1),
        minute(ist(9, 15), 54, 60, 53, 58, 2),
        minute(ist(15, 29), 58, 59, 40, 41, 3),
    };
    const auto days = aggregate(minutes, {.minutes = 1440, .daily = true});
    ASSERT_EQ(days.size(), 2u);
    EXPECT_EQ(days[1].start, kMidnightIst);
    EXPECT_EQ(days[1].low.raw(), 40);
    EXPECT_EQ(days[1].volume.value(), 5);
}

TEST(AggregateTest, OneMinuteIsUnchanged) {
    const std::vector<Bar> minutes{minute(ist(9, 15), 1, 2, 1, 2, 3)};
    EXPECT_EQ(aggregate(minutes, {.minutes = 1}).size(), 1u);
}

TEST(AggregateTest, OiUpdateTimeIsTheLastKnownOneInTheBucket) {
    std::vector<Bar> minutes{minute(ist(10, 0), 1, 1, 1, 1, 0, 100), minute(ist(10, 1), 1, 1, 1, 1, 0, 110),
                             minute(ist(10, 2), 1, 1, 1, 1, 0, 110), minute(ist(10, 4), 1, 1, 1, 1, 0, 120)};
    minutes[0].oi_update_offset = 10;
    minutes[1].oi_update_offset = 20;  // the last known one in 10:00–10:02
    minutes[3].oi_update_offset = 5;   // alone in its bucket, which starts a minute earlier
    const auto bars = aggregate(minutes, {.minutes = 3});
    ASSERT_EQ(bars.size(), 2u);
    EXPECT_EQ(bars[0].oi_update_offset, 80);
    EXPECT_EQ(bars[1].oi_update_offset, 65);
}

}  // namespace
}  // namespace mda::bars
