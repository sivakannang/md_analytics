#include <vector>

#include <gtest/gtest.h>

#include <mda/apps/backfill.hpp>

namespace mda::apps {
namespace {

constexpr std::int64_t kThursday = 1'792'607'400;  // 2026-10-22 00:00 IST
constexpr std::int64_t kSaturday = kThursday + 2 * 86'400;

std::int64_t at(std::int64_t day, int h, int m) {
    return day + h * 3600 + m * 60;
}

TEST(BackfillTest, MarketHoursAreWeekdays0915To1540Ist) {
    EXPECT_FALSE(in_market_hours(at(kThursday, 9, 14)));
    EXPECT_TRUE(in_market_hours(at(kThursday, 9, 15)));
    EXPECT_TRUE(in_market_hours(at(kThursday, 15, 39)));
    EXPECT_FALSE(in_market_hours(at(kThursday, 15, 40)));
    EXPECT_FALSE(in_market_hours(at(kSaturday, 11, 0)));
}

TEST(BackfillTest, DuringMarketHoursOnlyTodayIsFetched) {
    const std::int64_t now = at(kThursday, 11, 0);
    const auto window = auto_backfill_window(now, {.past = false, .today = false}, 30);
    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(window->from, at(kThursday, 9, 15));
    EXPECT_EQ(window->to, now);
    EXPECT_FALSE(auto_backfill_window(now, {.past = false, .today = true}, 30).has_value());
}

TEST(BackfillTest, AfterHoursThePastDaysAreFetched) {
    const std::int64_t now = at(kThursday, 22, 50);
    const auto window = auto_backfill_window(now, {.past = false, .today = true}, 30);
    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(window->from, at(kThursday - 29 * 86'400, 9, 15));  // 30 days including today
    EXPECT_EQ(window->to, now);
    EXPECT_FALSE(auto_backfill_window(now, {.past = true, .today = false}, 30).has_value());
    EXPECT_TRUE(auto_backfill_window(at(kSaturday, 11, 0), {}, 30).has_value());
}

bars::Bar bar(std::uint32_t id, std::int64_t start) {
    bars::Bar b{};
    b.header = messages::make_header<bars::Bar>(core::InstrumentId(id), 0);
    b.start = start;
    return b;
}

TEST(BackfillTest, CoverageLooksAtTodayAndThePastDays) {
    auto store = bars::BarStore::open_in_memory().value();
    ASSERT_TRUE(store.write(std::vector{bar(1, at(kThursday - 86'400, 15, 29)), bar(2, at(kThursday, 9, 15)),
                                        bar(3, at(kThursday - 40 * 86'400, 10, 0))})
                    .has_value());
    const std::int64_t now = at(kThursday, 22, 0);
    const auto one = bar_coverage(store, 1, now, 30).value();
    EXPECT_TRUE(one.past);
    EXPECT_FALSE(one.today);
    const auto two = bar_coverage(store, 2, now, 30).value();
    EXPECT_FALSE(two.past);
    EXPECT_TRUE(two.today);
    const auto three = bar_coverage(store, 3, now, 30).value();  // older than the window
    EXPECT_FALSE(three.past);
    EXPECT_FALSE(bar_coverage(store, 4, now, 30).value().today);
}

TEST(BackfillTest, CandlesBecomeBackfilledBarsUpToTheFormingMinute) {
    std::vector<kite::Candle> candles{{.start = 60, .close = core::Price::from_raw(10), .volume = 5, .oi = 100},
                                      {.start = 120, .close = core::Price::from_raw(11), .volume = 6, .oi = 100},
                                      {.start = 180, .close = core::Price::from_raw(12), .volume = 7, .oi = 120},
                                      {.start = 240, .close = core::Price::from_raw(13), .volume = 8, .oi = 120}};
    const auto bars = to_bars(core::InstrumentId(9), candles, 240);
    ASSERT_EQ(bars.size(), 3u);
    EXPECT_EQ(bars[0].source, bars::BarSource::Kite);
    EXPECT_EQ(bars[0].oi_updates, 0u);
    EXPECT_EQ(bars[1].oi_updates, 0u);
    EXPECT_EQ(bars[2].oi_updates, 1u);
    EXPECT_EQ(bars[2].oi_close.value(), 120);
    EXPECT_EQ(ist_text(kThursday + 9 * 3600 + 15 * 60), "2026-10-22 09:15:00");
}

}  // namespace
}  // namespace mda::apps
