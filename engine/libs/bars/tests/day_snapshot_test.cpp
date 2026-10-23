#include <vector>

#include <gtest/gtest.h>

#include <mda/bars/day_snapshot.hpp>

namespace mda::bars {
namespace {

Bar minute(std::int64_t start, std::int64_t o, std::int64_t h, std::int64_t l, std::int64_t c, std::int64_t volume,
           std::int64_t oi = 0) {
    Bar b{};
    b.header = messages::make_header<Bar>(core::InstrumentId(7), 0);
    b.start = start;
    b.open = core::Price::from_raw(o);
    b.high = core::Price::from_raw(h);
    b.low = core::Price::from_raw(l);
    b.close = core::Price::from_raw(c);
    b.volume = core::Quantity(volume);
    if (oi > 0) {
        b.oi_open = b.oi_low = core::Quantity(oi);
        b.oi_high = b.oi_close = core::Quantity(oi + 50);
    }
    return b;
}

TEST(DaySnapshotTest, SummarisesTheDayLikeAQuote) {
    const std::vector<Bar> day{minute(600, 100, 104, 99, 103, 10, 1'000), minute(660, 103, 110, 102, 108, 5),
                               minute(720, 108, 109, 95, 97, 7, 900)};
    const auto s = day_snapshot(day, core::Price::from_raw(101));
    EXPECT_EQ(s.header.instrument_id, core::InstrumentId(7));
    EXPECT_EQ(s.ltp, core::Price::from_raw(97));
    EXPECT_EQ(s.open, core::Price::from_raw(100));
    EXPECT_EQ(s.high, core::Price::from_raw(110));
    EXPECT_EQ(s.low, core::Price::from_raw(95));
    EXPECT_EQ(s.close, core::Price::from_raw(101));  // previous day's close
    EXPECT_EQ(s.volume.value(), 22);
    EXPECT_EQ(s.oi.value(), 950);
    EXPECT_EQ(s.oi_day_high.value(), 1'050);
    EXPECT_EQ(s.oi_day_low.value(), 900);
    EXPECT_NE(s.fields & messages::kFieldOpenInterest, 0u);
    EXPECT_EQ(s.exchange_ts, core::Timestamp::from_seconds(780));
}

TEST(DaySnapshotTest, CashHasNoOpenInterest) {
    const std::vector<Bar> day{minute(600, 100, 104, 99, 103, 10)};
    const auto s = day_snapshot(day, core::Price{});
    EXPECT_EQ(s.fields & messages::kFieldOpenInterest, 0u);
    EXPECT_EQ(s.close.raw(), 0);
}

}  // namespace
}  // namespace mda::bars
