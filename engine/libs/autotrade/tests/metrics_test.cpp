#include <gtest/gtest.h>

#include <mda/autotrade/metrics.hpp>

namespace mda::autotrade {
namespace {

broker::TradeRow fill(oms::Side side, std::int64_t qty, oms::Paise realised, oms::Paise brokerage,
                      Millis at) {
    return {.order_id = at,
            .account_id = 1,
            .instrument = core::InstrumentId(18486),
            .side = side,
            .product = oms::Product::Mis,
            .quantity = qty,
            .price = {},
            .charges = {.brokerage = brokerage},
            .realised = realised,
            .at = at};
}

// Four trips, worked by hand (paise): net +3,000, −5,000, +1,000, −4,000; one open short.
const broker::TradeRow kTrades[] = {
    fill(oms::Side::Sell, 65, 0, 1'000, 1),      // trip 1: short
    fill(oms::Side::Buy, 65, 5'000, 1'000, 2),   //   +5,000 − 2,000 = +3,000
    fill(oms::Side::Sell, 65, 0, 1'000, 3),      // trip 2
    fill(oms::Side::Buy, 65, -3'000, 1'000, 4),  //   −3,000 − 2,000 = −5,000
    fill(oms::Side::Buy, 65, 0, 1'000, 5),       // trip 3: long
    fill(oms::Side::Sell, 65, 3'000, 1'000, 6),  //   +1,000
    fill(oms::Side::Sell, 65, 0, 1'000, 7),      // trip 4
    fill(oms::Side::Buy, 65, -2'000, 1'000, 8),  //   −4,000
    fill(oms::Side::Sell, 65, 0, 1'000, 9),      // still open
};

TEST(Metrics, RoundTrips) {
    const auto trips = round_trips(kTrades);
    ASSERT_EQ(trips.size(), 4u);
    EXPECT_EQ(trips[0].quantity, -65);
    EXPECT_EQ(trips[0].net(), 3'000);
    EXPECT_EQ(trips[1].net(), -5'000);
    EXPECT_EQ(trips[2].quantity, 65);
    EXPECT_EQ(trips[2].opened, 5);
    EXPECT_EQ(trips[2].closed, 6);
}

TEST(Metrics, SummaryWorkedByHand) {
    const Decision decisions[] = {
        {.action = "exit", .reason = "target: +80.00 points"},
        {.action = "stopped out", .reason = "stop filled"},
        {.action = "exit", .reason = "signal changed: Neutral +5"},
        {.action = "exit", .reason = "max hold"},
        {.action = "blocked", .reason = "Bearish -60: cooldown"},
        {.action = "exit", .reason = "end of day"},
    };
    const auto m = metrics(kTrades, decisions);
    EXPECT_EQ(m.round_trips, 4);
    EXPECT_EQ(m.wins, 2);
    EXPECT_EQ(m.losses, 2);
    EXPECT_EQ(m.gross, 3'000);    // 5,000 − 3,000 + 3,000 − 2,000
    EXPECT_EQ(m.charges, 9'000);  // nine fills
    EXPECT_EQ(m.net, -6'000);
    EXPECT_EQ(m.average_win, 2'000);    // (3,000 + 1,000) / 2
    EXPECT_EQ(m.average_loss, -4'500);  // (−5,000 − 4,000) / 2
    EXPECT_EQ(m.largest_loss, -5'000);
    // Cumulative net: 3,000 → −2,000 → −1,000 → −5,000; from the 3,000 peak down 8,000.
    EXPECT_EQ(m.max_drawdown, 8'000);
    EXPECT_EQ(m.open_position, -65);
    EXPECT_EQ(m.exits.at("target"), 1);
    EXPECT_EQ(m.exits.at("stop"), 1);
    EXPECT_EQ(m.exits.at("signal"), 1);
    EXPECT_EQ(m.exits.at("max hold"), 1);
    EXPECT_EQ(m.exits.at("end of day"), 1);
    EXPECT_FALSE(m.exits.contains("blocked"));
}

TEST(Metrics, NothingTraded) {
    const auto m = metrics({}, {});
    EXPECT_EQ(m.round_trips, 0);
    EXPECT_EQ(m.net, 0);
    EXPECT_EQ(m.max_drawdown, 0);
}

}  // namespace
}  // namespace mda::autotrade
