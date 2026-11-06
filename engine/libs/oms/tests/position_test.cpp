#include <gtest/gtest.h>

#include <mda/oms/position.hpp>

namespace mda::oms {
namespace {

core::Price rupees(double value) {
    return core::Price::from_raw(static_cast<std::int64_t>(value * core::Price::kScale + 0.5));
}

TEST(Position, AddingAveragesAndReducingRealises) {
    Position p;
    EXPECT_EQ(p.apply(Side::Buy, 100, rupees(10)), 0);
    EXPECT_EQ(p.apply(Side::Buy, 100, rupees(12)), 0);
    EXPECT_EQ(p.average_price(), rupees(11));
    EXPECT_EQ(p.apply(Side::Sell, 50, rupees(15)), 20'000);  // (15 - 11) × 50 = ₹200
    EXPECT_EQ(p.quantity(), 150);
    EXPECT_EQ(p.average_price(), rupees(11));                // reducing keeps the average
    EXPECT_EQ(p.unrealised(rupees(12)), 15'000);
    EXPECT_EQ(p.notional(rupees(12)), 180'000);
}

TEST(Position, ThroughZeroOpensTheRestAtTheFillPrice) {
    Position p;
    p.apply(Side::Buy, 150, rupees(11));
    EXPECT_EQ(p.apply(Side::Sell, 200, rupees(14)), 45'000);  // 150 closed at +₹3
    EXPECT_EQ(p.quantity(), -50);
    EXPECT_EQ(p.average_price(), rupees(14));
    EXPECT_EQ(p.unrealised(rupees(13)), 5'000);                // short gains as it falls
    EXPECT_EQ(p.apply(Side::Buy, 50, rupees(13.5)), 2'500);
    EXPECT_EQ(p.quantity(), 0);
    EXPECT_EQ(p.average_price(), core::Price{});
    EXPECT_EQ(p.unrealised(rupees(99)), 0);
    EXPECT_EQ(p.realised(), 47'500);
}

TEST(Position, ShortAddsAndCovers) {
    Position p;
    p.apply(Side::Sell, 75, rupees(22'500));
    p.apply(Side::Sell, 75, rupees(22'520));
    EXPECT_EQ(p.average_price(), rupees(22'510));
    EXPECT_EQ(p.apply(Side::Buy, 75, rupees(22'480)), 225'000);  // ₹30 × 75
    EXPECT_EQ(p.quantity(), -75);
}

TEST(Position, UnevenAveragesStayExactOverAFullClose) {
    Position p;
    p.apply(Side::Buy, 1, rupees(10.05));
    p.apply(Side::Buy, 2, rupees(10.10));
    EXPECT_EQ(p.average_price().raw(), 100'833);
    // Closing everything realises exactly 3 × 10.20 − (10.05 + 2 × 10.10) = ₹0.35.
    EXPECT_EQ(p.apply(Side::Sell, 3, rupees(10.20)), 35);
}

TEST(Position, TheEntryPriceIgnoresMarks) {
    Position p;
    p.apply(Side::Sell, 100, rupees(9554));
    p.apply(Side::Sell, 100, rupees(9534));
    EXPECT_EQ(p.entry_price(), rupees(9544));
    p.mark(rupees(9530));
    p.mark(rupees(9802));
    EXPECT_EQ(p.average_price(), rupees(9802));
    EXPECT_EQ(p.entry_price(), rupees(9544));
    p.apply(Side::Buy, 100, rupees(9000));  // reducing keeps it
    EXPECT_EQ(p.quantity(), -100);
    EXPECT_EQ(p.entry_price(), rupees(9544));
    p.apply(Side::Buy, 150, rupees(9100));  // through zero: the rest entered at the fill
    EXPECT_EQ(p.quantity(), 50);
    EXPECT_EQ(p.entry_price(), rupees(9100));
    p.apply(Side::Sell, 50, rupees(9200));
    EXPECT_EQ(p.entry_price(), core::Price{});
}

TEST(Position, EntryPnlCountsFromTheEntryPrice) {
    Position p;
    p.apply(Side::Buy, 100, rupees(100));
    p.mark(rupees(110));
    EXPECT_EQ(p.entry_pnl(Side::Buy, 100, rupees(120)), 0);  // adding closes nothing
    EXPECT_EQ(p.entry_pnl(Side::Sell, 40, rupees(120)), 80'000);    // ₹20 × 40
    EXPECT_EQ(p.entry_pnl(Side::Sell, 150, rupees(120)), 200'000);  // only the 100 held
}

TEST(Position, ConversionMovesTheEntryPrice) {
    Position mis;
    mis.apply(Side::Buy, 100, rupees(100));
    mis.mark(rupees(110));
    Position nrml;
    nrml.apply(Side::Buy, 100, rupees(200));
    const Position::Taken taken = mis.take(100);
    EXPECT_EQ(nrml.put(100, taken), 0);
    EXPECT_EQ(nrml.average_price(), rupees(155));
    EXPECT_EQ(nrml.entry_price(), rupees(150));

    Position short_side;
    short_side.apply(Side::Sell, 50, rupees(300));
    (void)short_side.put(100, Position::Taken{.cost_raw = rupees(110).raw() * 100, .entry_cost_raw = rupees(100).raw() * 100});
    EXPECT_EQ(short_side.quantity(), 50);  // the 50 left keep the entry they came with
    EXPECT_EQ(short_side.average_price(), rupees(110));
    EXPECT_EQ(short_side.entry_price(), rupees(100));
}

}  // namespace
}  // namespace mda::oms
