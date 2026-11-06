#include <initializer_list>
#include <utility>

#include <gtest/gtest.h>

#include <mda/oms/paper_venue.hpp>

namespace mda::oms {
namespace {

core::Price rupees(double value) {
    return core::Price::from_raw(static_cast<std::int64_t>(value * core::Price::kScale + 0.5));
}

constexpr Millis kT0 = 1'792'640'000'000;  // accepted here
constexpr Millis kLive = kT0 + 100;        // eligible (100 ms latency)

/// A fresh book: LTP, bids and asks as {price, quantity} (best first).
Book book(double ltp, std::initializer_list<std::pair<double, std::int64_t>> bids,
          std::initializer_list<std::pair<double, std::int64_t>> asks, Millis at = kLive) {
    Book b{.ltp = rupees(ltp),
           .ltp_at = at,
           .bids = {},
           .asks = {},
           .updated_at = at,
           .tick = rupees(0.05),
           .feed_ok = true};
    std::size_t i = 0;
    for (const auto& [price, qty] : bids) {
        b.bids[i++] = {.price = rupees(price), .quantity = qty};
    }
    i = 0;
    for (const auto& [price, qty] : asks) {
        b.asks[i++] = {.price = rupees(price), .quantity = qty};
    }
    return b;
}

Order order(Side side, OrderType type, std::int64_t qty, double price = 0, double trigger = 0) {
    return Order::accepted(1,
                           {.account = 1,
                            .client_order_id = 1,
                            .source = Source::Ui,
                            .instrument = core::InstrumentId(9),
                            .side = side,
                            .type = type,
                            .product = Product::Mis,
                            .quantity = qty,
                            .price = rupees(price),
                            .trigger = rupees(trigger),
                            .tag = {}},
                           kT0);
}

const Book kMarket =
    book(100.00, {{99.95, 40}, {99.90, 60}}, {{100.00, 50}, {100.05, 50}, {100.10, 100}});

TEST(PaperVenue, NothingBeforeTheLatency) {
    PaperVenue venue;
    const auto o = order(Side::Buy, OrderType::Market, 20);
    venue.accepted(o, kT0);
    EXPECT_TRUE(venue.evaluate(o, kMarket, kT0 + 50).fills.empty());
    EXPECT_EQ(venue.evaluate(o, kMarket, kLive).fills.size(), 1u);
}

TEST(PaperVenue, MarketBuyWalksTheAsks) {
    PaperVenue venue;
    const auto o = order(Side::Buy, OrderType::Market, 120);
    venue.accepted(o, kT0);
    const auto r = venue.evaluate(o, kMarket, kLive);
    ASSERT_EQ(r.fills.size(), 3u);
    EXPECT_EQ(r.fills[0].quantity, 50);
    EXPECT_EQ(r.fills[0].price, rupees(100.00));
    EXPECT_EQ(r.fills[1].price, rupees(100.05));
    EXPECT_EQ(r.fills[2].quantity, 20);
    EXPECT_EQ(r.fills[2].price, rupees(100.10));
    EXPECT_FALSE(r.reject.has_value());
}

TEST(PaperVenue, MarketBeyondTheBookSlipsTwoTicks) {
    PaperVenue venue;
    const auto buy = order(Side::Buy, OrderType::Market, 100);
    venue.accepted(buy, kT0);
    const auto thin = book(100, {{99.95, 10}, {99.90, 20}}, {{100.00, 10}, {100.05, 20}});
    const auto r = venue.evaluate(buy, thin, kLive);
    ASSERT_EQ(r.fills.size(), 3u);
    EXPECT_EQ(r.fills[2].quantity, 70);
    EXPECT_EQ(r.fills[2].price, rupees(100.15));

    PaperVenue sells;
    const auto sell = order(Side::Sell, OrderType::Market, 100);
    sells.accepted(sell, kT0);
    const auto s = sells.evaluate(sell, thin, kLive);
    ASSERT_EQ(s.fills.size(), 3u);
    EXPECT_EQ(s.fills[0].price, rupees(99.95));
    EXPECT_EQ(s.fills[2].price, rupees(99.80));
}

TEST(PaperVenue, MarketRejectedWithoutLiquidityOrFreshData) {
    const auto o = order(Side::Buy, OrderType::Market, 10);
    PaperVenue venue;
    venue.accepted(o, kT0);
    EXPECT_EQ(venue.evaluate(o, book(100, {{99.95, 10}}, {}), kLive).reject,
              "no liquidity on the other side");

    Book stale = kMarket;
    stale.updated_at = kLive - 6'000;
    EXPECT_EQ(venue.evaluate(o, stale, kLive).reject, "no fresh market data");
    Book down = kMarket;
    down.feed_ok = false;
    EXPECT_EQ(venue.evaluate(o, down, kLive).reject, "no fresh market data");
}

TEST(PaperVenue, MarketableLimitTakesThenRestsUntilQuotedThrough) {
    PaperVenue venue;
    auto o = order(Side::Buy, OrderType::Limit, 150, 100.05);
    venue.accepted(o, kT0);
    const auto first = venue.evaluate(o, kMarket, kLive);
    ASSERT_EQ(first.fills.size(), 2u);  // 50 @ 100.00, 50 @ 100.05; never 100.10
    ASSERT_TRUE(o.fill(50, first.fills[0].price, kLive).has_value());
    ASSERT_TRUE(o.fill(50, first.fills[1].price, kLive).has_value());

    // Resting now: the same book (ask at 100.10) fills nothing more.
    const auto same = book(100.05, {{100.00, 10}}, {{100.10, 100}}, kLive + 1'000);
    EXPECT_TRUE(venue.evaluate(o, same, kLive + 1'000).fills.empty());
    // An ask at the limit only touches it; an ask below it is through it.
    EXPECT_TRUE(venue.evaluate(o, book(100.05, {}, {{100.05, 100}}, kLive + 2'000), kLive + 2'000)
                    .fills.empty());
    const auto through =
        venue.evaluate(o, book(100.05, {}, {{100.00, 100}}, kLive + 3'000), kLive + 3'000);
    ASSERT_EQ(through.fills.size(), 1u);
    EXPECT_EQ(through.fills[0].quantity, 50);
    EXPECT_EQ(through.fills[0].price, rupees(100.05));  // at its own price
}

TEST(PaperVenue, RestingLimitFillsOnlyWhenTradedThrough) {
    PaperVenue venue;
    const auto o = order(Side::Buy, OrderType::Limit, 75, 99.50);
    venue.accepted(o, kT0);
    EXPECT_TRUE(
        venue.evaluate(o, kMarket, kLive).fills.empty());  // its first look: nothing at 99.50

    const auto touch = book(99.50, {{99.45, 10}}, {{99.55, 10}}, kLive + 1'000);
    EXPECT_TRUE(venue.evaluate(o, touch, kLive + 1'000).fills.empty());
    // A trade below the limit, but before the order could have been there: ignored.
    Book early = book(99.40, {{99.45, 10}}, {{99.55, 10}}, kLive + 2'000);
    early.ltp_at = kT0;
    EXPECT_TRUE(venue.evaluate(o, early, kLive + 2'000).fills.empty());
    const auto r =
        venue.evaluate(o, book(99.45, {{99.40, 10}}, {{99.55, 10}}, kLive + 3'000), kLive + 3'000);
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].quantity, 75);
    EXPECT_EQ(r.fills[0].price, rupees(99.50));
}

TEST(PaperVenue, RestingSellLimitMirrors) {
    PaperVenue venue;
    const auto o = order(Side::Sell, OrderType::Limit, 75, 100.50);
    venue.accepted(o, kT0);
    EXPECT_TRUE(venue.evaluate(o, kMarket, kLive).fills.empty());
    const auto r = venue.evaluate(o, book(100.55, {{100.50, 10}}, {{100.60, 10}}, kLive + 1'000),
                                  kLive + 1'000);
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].price, rupees(100.50));
}

TEST(PaperVenue, StopMarketTriggersThenFillsAfterTheLatency) {
    PaperVenue venue;
    auto o = order(Side::Sell, OrderType::StopMarket, 75, 0, 99.00);  // stop loss on a long
    venue.accepted(o, kT0);
    EXPECT_FALSE(venue.evaluate(o, kMarket, kLive).triggered);
    const Millis t = kLive + 1'000;
    const auto hit = book(98.95, {{98.90, 100}}, {{99.00, 100}}, t);
    const auto r = venue.evaluate(o, hit, t);
    EXPECT_TRUE(r.triggered);
    EXPECT_TRUE(r.fills.empty());
    ASSERT_TRUE(o.trigger(t).has_value());
    EXPECT_TRUE(venue.evaluate(o, hit, t + 50).fills.empty());  // latency again
    const auto f = venue.evaluate(o, hit, t + 100);
    ASSERT_EQ(f.fills.size(), 1u);
    EXPECT_EQ(f.fills[0].price, rupees(98.90));
}

TEST(PaperVenue, StopLimitTakesUpToItsPrice) {
    PaperVenue venue;
    auto o = order(Side::Buy, OrderType::StopLimit, 100, 101.20, 101.00);
    venue.accepted(o, kT0);
    const Millis t = kLive + 1'000;
    const auto up = book(101.05, {{101.00, 50}}, {{101.10, 40}, {101.20, 40}, {101.25, 100}}, t);
    ASSERT_TRUE(venue.evaluate(o, up, t).triggered);
    ASSERT_TRUE(o.trigger(t).has_value());
    const auto r = venue.evaluate(o, up, t + 100);
    ASSERT_EQ(r.fills.size(), 2u);  // 40 @ 101.10, 40 @ 101.20; 20 rest at 101.20
    EXPECT_EQ(r.fills[1].price, rupees(101.20));
}

TEST(PaperVenue, StaleBookFillsNothingAndFinalOrdersAreIgnored) {
    PaperVenue venue;
    const auto o = order(Side::Buy, OrderType::Limit, 75, 99.50);
    venue.accepted(o, kT0);
    EXPECT_TRUE(venue.evaluate(o, kMarket, kLive).fills.empty());
    Book stale = book(99.00, {}, {{99.00, 100}}, kLive + 1'000);
    stale.updated_at = kLive - 10'000;
    const auto r = venue.evaluate(o, stale, kLive + 1'000);
    EXPECT_TRUE(r.fills.empty());
    EXPECT_FALSE(r.reject.has_value());  // a resting order waits

    auto done = order(Side::Buy, OrderType::Market, 10);
    ASSERT_TRUE(done.cancel("user", kT0).has_value());
    EXPECT_TRUE(venue.evaluate(done, kMarket, kLive).fills.empty());
    venue.forget(o.id());
}

}  // namespace
}  // namespace mda::oms
