#include <vector>

#include <gtest/gtest.h>

#include <mda/bars/bar_store.hpp>

namespace mda::bars {
namespace {

Bar bar(std::uint32_t id, std::int64_t start, std::int64_t close,
        BarSource source = BarSource::Live) {
    Bar b{};
    b.header = messages::make_header<Bar>(core::InstrumentId(id), 0);
    b.start = start;
    b.open = b.high = b.low = b.close = core::Price::from_raw(close);
    b.volume = core::Quantity(10);
    b.oi_close = core::Quantity(500);
    b.oi_updates = 2;
    b.oi_update_offset = 42;
    b.source = source;
    return b;
}

TEST(BarStoreTest, WritesAndReadsRanges) {
    auto store = BarStore::open_in_memory().value();
    const std::vector<Bar> bars{bar(7, 60, 100), bar(7, 120, 101), bar(7, 180, 102),
                                bar(8, 60, 500)};
    ASSERT_TRUE(store.write(bars).has_value());

    const auto range = store.range(7, 60, 180).value();
    ASSERT_EQ(range.size(), 2u);  // [from, to)
    EXPECT_EQ(range[1].close, core::Price::from_raw(101));
    EXPECT_EQ(range[1].oi_updates, 2u);
    EXPECT_EQ(range[1].oi_update_offset, 42);
    EXPECT_EQ(range[1].header.instrument_id, core::InstrumentId(7));

    const auto latest = store.latest_per_instrument(0, 1'000).value();
    ASSERT_EQ(latest.size(), 2u);
    for (const Bar& b : latest) {
        EXPECT_EQ(b.start, b.header.instrument_id == core::InstrumentId(7) ? 180 : 60);
    }
}

TEST(BarStoreTest, FindsTheLastBarBeforeATime) {
    auto store = BarStore::open_in_memory().value();
    ASSERT_TRUE(store.write(std::vector<Bar>{bar(7, 60, 100), bar(7, 120, 101), bar(8, 60, 500)}).has_value());
    EXPECT_EQ(store.latest_before(7, 120).value()->close, core::Price::from_raw(100));
    EXPECT_EQ(store.latest_before(7, 1'000).value()->start, 120);
    EXPECT_FALSE(store.latest_before(7, 60).value().has_value());
}

TEST(BarStoreTest, FindsEachInstrumentsLastBarEndedByATime) {
    auto store = BarStore::open_in_memory().value();
    Bar no_oi = bar(7, 180, 103);
    no_oi.oi_close = core::Quantity(0);
    ASSERT_TRUE(store.write(std::vector<Bar>{bar(7, 60, 100), bar(7, 120, 101), no_oi, bar(8, 60, 500)}).has_value());
    const std::vector<std::uint32_t> ids{7, 8, 9};

    // At 180 the bar starting at 180 hasn't ended: 7's is the one at 120.
    auto at = store.last_ended(ids, 180, 0, true).value();
    EXPECT_EQ(at.size(), 2u);  // 9 has no bars
    EXPECT_EQ(at.at(7).start, 120);
    EXPECT_EQ(at.at(8).start, 60);

    // At 240 the bar at 180 has ended, but carries no OI: skipped only when OI is asked for.
    EXPECT_EQ(store.last_ended(ids, 240, 0, true).value().at(7).start, 120);
    EXPECT_EQ(store.last_ended(ids, 240, 0, false).value().at(7).start, 180);

    // A live bar that only carries OI forward (no update in it) is skipped; a backfilled one isn't.
    Bar carried = bar(8, 120, 501);
    carried.oi_updates = 0;
    Bar backfilled = bar(9, 60, 1, BarSource::Kite);
    backfilled.oi_updates = 0;
    ASSERT_TRUE(store.write(std::vector<Bar>{carried, backfilled}).has_value());
    at = store.last_ended(ids, 240, 0, true).value();
    EXPECT_EQ(at.at(8).start, 60);
    EXPECT_EQ(at.at(9).start, 60);

    // Nothing ended by 60 + 59, and nothing at or after `earliest`.
    EXPECT_TRUE(store.last_ended(ids, 119, 0, true).value().empty());
    EXPECT_FALSE(store.last_ended(ids, 240, 121, true).value().contains(7));
}

TEST(BarStoreTest, SumsVolumePerInstrument) {
    auto store = BarStore::open_in_memory().value();
    ASSERT_TRUE(store.write(std::vector{bar(7, 60, 1), bar(7, 120, 1), bar(8, 60, 1), bar(7, 600, 1)}).has_value());
    const auto volumes = store.volumes(0, 600).value();
    EXPECT_EQ(volumes.size(), 2u);
    EXPECT_EQ(volumes.at(7), 20);  // the bar at 600 is outside [0, 600)
    EXPECT_EQ(volumes.at(8), 10);
}

TEST(BarStoreTest, LiveBarsWinOverBackfill) {
    auto store = BarStore::open_in_memory().value();
    ASSERT_TRUE(store.write(std::vector{bar(7, 60, 100, BarSource::Kite)}).has_value());
    ASSERT_TRUE(
        store.write(std::vector{bar(7, 60, 111, BarSource::Live)}).has_value());  // replaces
    ASSERT_TRUE(store.write(std::vector{bar(7, 60, 122, BarSource::Kite)}).has_value());  // ignored
    const auto stored = store.range(7, 0, 1'000).value();
    ASSERT_EQ(stored.size(), 1u);
    EXPECT_EQ(stored[0].close, core::Price::from_raw(111));
    EXPECT_EQ(stored[0].source, BarSource::Live);

    ASSERT_TRUE(store.write(std::vector{bar(9, 60, 1, BarSource::Kite)}).has_value());
    ASSERT_TRUE(store.write(std::vector{bar(9, 60, 2, BarSource::Kite)})
                    .has_value());  // backfill refreshes backfill
    EXPECT_EQ(store.range(9, 0, 1'000).value()[0].close, core::Price::from_raw(2));
}

}  // namespace
}  // namespace mda::bars
